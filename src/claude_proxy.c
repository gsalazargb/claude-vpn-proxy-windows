/*
 * claude_proxy.c
 *
 * Proxy HTTP local minimalista para Windows. Recibe conexiones de UNA sola
 * app a la vez (Claude Desktop, o lo que sea que se lance apuntando
 * HTTPS_PROXY/--proxy-server a este puerto) y fuerza la salida de esas
 * conexiones por una interfaz de red especifica (el tunel WireGuard
 * dedicado, de cualquier proveedor compatible con WireGuard: Proton,
 * Mullvad, un servidor propio, etc) usando IP_UNICAST_IF, sin necesitar
 * ningun driver de kernel.
 *
 * Soporta:
 *   - CONNECT host:port HTTP/1.1   (la inmensa mayoria del trafico, HTTPS)
 *   - Peticiones HTTP planas con URL completa en la request-line (fallback)
 *
 * Uso:
 *   claude_proxy.exe <puerto_local> <nombre_adaptador>
 *   claude_proxy.exe 18080 ClaudeVPN
 *
 * El nombre del adaptador es el FriendlyName que Windows le puso a la
 * interfaz del tunel WireGuard (por defecto, el mismo nombre del archivo
 * .conf sin extension, ej. "ClaudeVPN.conf" -> adaptador "ClaudeVPN").
 *
 * Limitacion conocida: la resolucion DNS de los hostnames (ej.
 * api.anthropic.com) se hace por el resolver normal del sistema, NO a
 * traves del tunel. Solo el trafico de datos en si sale por la VPN dedicada.
 *
 * Ciclo de vida: se compila sin consola (-mwindows), asi que no abre ninguna
 * ventana; los mensajes van a %LOCALAPPDATA%\ClaudeVPNProxy\claude_proxy.log.
 * Lo arranca el acceso directo "Claude (VPN)" justo antes
 * de abrir la app. Por cada conexion entrante se averigua que proceso la
 * abrio (GetExtendedTcpTable) y el proxy se cierra solo cuando todos esos
 * procesos terminaron y no queda ninguna conexion abierta. Si nadie se
 * conecta en STARTUP_GRACE_MS, tambien se cierra.
 */

#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LISTEN_BACKLOG     64
#define BUF_SIZE           8192
#define LINE_BUF_SIZE      4096

#define STARTUP_GRACE_MS   120000 /* nadie se conecto en 2 min: salir */
#define IDLE_GRACE_MS      10000  /* clientes cerrados hace 10 s: salir */
#define MONITOR_POLL_MS    2000
#define MAX_TRACKED        64

static DWORD g_boundIfIndex = 0; /* 0 = sin interfaz resuelta aun */
static char  g_adapterName[256];
static u_short g_listenPortNet;  /* puerto local en orden de red */

/* Procesos cliente vigilados: el proxy vive mientras alguno siga vivo */
static CRITICAL_SECTION g_trackLock;
static HANDLE g_trackedHandles[MAX_TRACKED];
static DWORD  g_trackedPids[MAX_TRACKED];
static int    g_trackedCount = 0;
static volatile LONG g_activeConns = 0;
static volatile LONG g_everConnected = 0;

/* ---------------------------------------------------------------------- */
/* Resolver el IfIndex IPv4 de la interfaz por su FriendlyName             */
/* ---------------------------------------------------------------------- */
static int resolve_adapter_index(const char *friendlyNameUtf8, DWORD *outIfIndex)
{
    ULONG size = 0;
    DWORD ret;
    IP_ADAPTER_ADDRESSES *addrs = NULL;
    IP_ADAPTER_ADDRESSES *cur;
    WCHAR wantedName[256];

    MultiByteToWideChar(CP_UTF8, 0, friendlyNameUtf8, -1, wantedName, 256);

    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, NULL, &size);
    if (ret != ERROR_BUFFER_OVERFLOW) {
        fprintf(stderr, "[!] GetAdaptersAddresses (sizing) fallo: %lu\n", ret);
        return 0;
    }

    addrs = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!addrs) return 0;

    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, addrs, &size);
    if (ret != NO_ERROR) {
        fprintf(stderr, "[!] GetAdaptersAddresses fallo: %lu\n", ret);
        free(addrs);
        return 0;
    }

    for (cur = addrs; cur != NULL; cur = cur->Next) {
        if (_wcsicmp(cur->FriendlyName, wantedName) == 0) {
            *outIfIndex = cur->IfIndex;
            free(addrs);
            return 1;
        }
    }

    free(addrs);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Conectar hacia destino, forzando salida por g_boundIfIndex              */
/* ---------------------------------------------------------------------- */
static SOCKET connect_bound(const char *host, const char *port)
{
    struct addrinfo hints, *res, *rp;
    SOCKET s = INVALID_SOCKET;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* IP_UNICAST_IF v4 solamente, alcanza para el caso de uso */
    hints.ai_socktype = SOCK_STREAM;

    err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[!] getaddrinfo(%s) fallo: %d\n", host, err);
        return INVALID_SOCKET;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;

        if (g_boundIfIndex != 0) {
            /* IP_UNICAST_IF espera el indice en orden de red (htonl), peculiaridad documentada de Winsock */
            DWORD ifIndexNetOrder = htonl(g_boundIfIndex);
            if (setsockopt(s, IPPROTO_IP, IP_UNICAST_IF, (const char *)&ifIndexNetOrder, sizeof(ifIndexNetOrder)) != 0) {
                fprintf(stderr, "[!] setsockopt(IP_UNICAST_IF) fallo: %d\n", WSAGetLastError());
            }
        }

        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0)
            break;

        /* Util para diagnosticar, por ejemplo, el firewall/kill-switch del
           cliente oficial de Proton bloqueando esta interfaz (502 sin pista
           de por que, visto en pruebas reales antes de cerrar ese cliente). */
        fprintf(stderr, "[!] connect(%s:%s) fallo: %d\n", host, port, WSAGetLastError());
        closesocket(s);
        s = INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return s;
}

/* ---------------------------------------------------------------------- */
/* Relay bidireccional entre dos sockets (un hilo por direccion)           */
/* ---------------------------------------------------------------------- */
typedef struct {
    SOCKET from;
    SOCKET to;
} RelayArgs;

static DWORD WINAPI relay_thread(LPVOID param)
{
    RelayArgs *ra = (RelayArgs *)param;
    char buf[BUF_SIZE];
    int n;

    while ((n = recv(ra->from, buf, sizeof(buf), 0)) > 0) {
        int sent = 0;
        while (sent < n) {
            int s = send(ra->to, buf + sent, n - sent, 0);
            if (s <= 0) goto done;
            sent += s;
        }
    }

done:
    shutdown(ra->to, SD_BOTH);
    shutdown(ra->from, SD_BOTH);
    free(ra);
    return 0;
}

static void start_relay(SOCKET a, SOCKET b)
{
    RelayArgs *ab = (RelayArgs *)malloc(sizeof(RelayArgs));
    RelayArgs *ba = (RelayArgs *)malloc(sizeof(RelayArgs));
    HANDLE h1, h2;

    ab->from = a; ab->to = b;
    ba->from = b; ba->to = a;

    h1 = CreateThread(NULL, 0, relay_thread, ab, 0, NULL);
    h2 = CreateThread(NULL, 0, relay_thread, ba, 0, NULL);

    if (h1) WaitForSingleObject(h1, INFINITE);
    if (h2) WaitForSingleObject(h2, INFINITE);
    if (h1) CloseHandle(h1);
    if (h2) CloseHandle(h2);

    closesocket(a);
    closesocket(b);
}

/* ---------------------------------------------------------------------- */
/* Parseo minimo de la primera linea de la peticion del cliente            */
/* ---------------------------------------------------------------------- */
static int recv_line(SOCKET s, char *buf, int maxlen)
{
    int total = 0;
    char c;
    while (total < maxlen - 1) {
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return total > 0 ? total : -1;
        if (c == '\n') break;
        if (c != '\r') buf[total++] = c;
    }
    buf[total] = '\0';
    return total;
}

/* Consume el resto de los headers hasta la linea vacia (no nos importa su contenido) */
static void consume_headers(SOCKET s)
{
    char line[LINE_BUF_SIZE];
    for (;;) {
        int n = recv_line(s, line, sizeof(line));
        if (n <= 0) break;
    }
}

static void handle_client(SOCKET client)
{
    char line[LINE_BUF_SIZE];
    char method[16], target[2048], httpver[16];
    char host[256], portStr[16];
    SOCKET upstream;

    if (recv_line(client, line, sizeof(line)) <= 0) {
        closesocket(client);
        return;
    }

    if (sscanf(line, "%15s %2047s %15s", method, target, httpver) != 3) {
        closesocket(client);
        return;
    }

    if (_stricmp(method, "CONNECT") == 0) {
        /* target = host:port */
        char *colon = strrchr(target, ':');
        if (!colon) { closesocket(client); return; }
        *colon = '\0';
        strncpy(host, target, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        strncpy(portStr, colon + 1, sizeof(portStr) - 1);
        portStr[sizeof(portStr) - 1] = '\0';

        consume_headers(client);

        upstream = connect_bound(host, portStr);
        if (upstream == INVALID_SOCKET) {
            const char *resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            closesocket(client);
            return;
        }

        {
            const char *resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
        }

        start_relay(client, upstream);
        return;
    }

    /* Fallback: HTTP plano con URL completa en la request-line */
    if (strncmp(target, "http://", 7) == 0) {
        char *p = target + 7;
        char *slash = strchr(p, '/');
        char *hostport = p;
        char hostportBuf[300];
        char *colon;
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len >= sizeof(hostportBuf)) { closesocket(client); return; }
        memcpy(hostportBuf, hostport, len);
        hostportBuf[len] = '\0';

        colon = strchr(hostportBuf, ':');
        if (colon) {
            size_t hostLen = (size_t)(colon - hostportBuf);
            if (hostLen >= sizeof(host) || strlen(colon + 1) >= sizeof(portStr)) {
                const char *resp = "HTTP/1.1 414 URI Too Long\r\n\r\n";
                send(client, resp, (int)strlen(resp), 0);
                closesocket(client);
                return;
            }
            memcpy(host, hostportBuf, hostLen);
            host[hostLen] = '\0';
            strcpy(portStr, colon + 1);
        } else {
            if (strlen(hostportBuf) >= sizeof(host)) {
                const char *resp = "HTTP/1.1 414 URI Too Long\r\n\r\n";
                send(client, resp, (int)strlen(resp), 0);
                closesocket(client);
                return;
            }
            strcpy(host, hostportBuf);
            strcpy(portStr, "80");
        }

        upstream = connect_bound(host, portStr);
        if (upstream == INVALID_SOCKET) {
            const char *resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            closesocket(client);
            return;
        }

        /* Reenviar la request-line original tal cual (sin la parte host:port) y las cabeceras */
        {
            char reqline[LINE_BUF_SIZE];
            const char *path = slash ? slash : "/";
            _snprintf(reqline, sizeof(reqline), "%s %s %s\r\n", method, path, httpver);
            send(upstream, reqline, (int)strlen(reqline), 0);
        }

        /* No tenemos las cabeceras guardadas (se consumirian con consume_headers),
           asi que en este fallback simple asumimos que el cliente no manda mas
           que la linea de peticion y pasamos directo al relay bidireccional. */
        start_relay(client, upstream);
        return;
    }

    {
        const char *resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(client, resp, (int)strlen(resp), 0);
    }
    closesocket(client);
}

/* ---------------------------------------------------------------------- */
/* Vigilancia de los procesos cliente (para cerrarse solo)                 */
/* ---------------------------------------------------------------------- */

/* PID del proceso dueno del otro extremo de una conexion local, 0 si no se
   pudo determinar. Busca en la tabla TCP la conexion cuyo puerto local es
   el puerto de origen del cliente y cuyo puerto remoto es el del proxy. */
static DWORD client_pid(SOCKET s)
{
    struct sockaddr_in peer;
    int peerLen = sizeof(peer);
    MIB_TCPTABLE_OWNER_PID *table = NULL;
    DWORD size = 0, pid = 0, i;
    int tries;

    if (getpeername(s, (struct sockaddr *)&peer, &peerLen) != 0)
        return 0;

    for (tries = 0; tries < 3; tries++) {
        DWORD ret = GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (ret == NO_ERROR) break;
        free(table);
        table = NULL;
        if (ret != ERROR_INSUFFICIENT_BUFFER) return 0;
        table = (MIB_TCPTABLE_OWNER_PID *)malloc(size);
        if (!table) return 0;
    }
    if (!table) return 0;

    for (i = 0; i < table->dwNumEntries; i++) {
        MIB_TCPROW_OWNER_PID *row = &table->table[i];
        if ((u_short)row->dwLocalPort == peer.sin_port &&
            (u_short)row->dwRemotePort == g_listenPortNet &&
            row->dwLocalAddr == peer.sin_addr.s_addr) {
            pid = row->dwOwningPid;
            break;
        }
    }
    free(table);
    return pid;
}

static void track_client_process(SOCKET s)
{
    DWORD pid = client_pid(s);
    HANDLE h;
    int i;

    if (pid == 0) {
        fprintf(stderr, "[!] no se pudo identificar el proceso de una conexion entrante\n");
        return;
    }

    EnterCriticalSection(&g_trackLock);
    for (i = 0; i < g_trackedCount; i++) {
        if (g_trackedPids[i] == pid) {
            LeaveCriticalSection(&g_trackLock);
            return;
        }
    }
    if (g_trackedCount < MAX_TRACKED) {
        /* SYNCHRONIZE: solo permite esperar a que el proceso termine */
        h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) {
            g_trackedHandles[g_trackedCount] = h;
            g_trackedPids[g_trackedCount] = pid;
            g_trackedCount++;
            fprintf(stderr, "[*] vigilando proceso cliente PID %lu\n", pid);
        }
    }
    LeaveCriticalSection(&g_trackLock);
}

static DWORD WINAPI monitor_thread(LPVOID param)
{
    ULONGLONG start = GetTickCount64();
    ULONGLONG idleSince = 0;
    (void)param;

    for (;;) {
        int alive, i;
        ULONGLONG now;

        Sleep(MONITOR_POLL_MS);
        now = GetTickCount64();

        EnterCriticalSection(&g_trackLock);
        for (i = 0; i < g_trackedCount; ) {
            if (WaitForSingleObject(g_trackedHandles[i], 0) == WAIT_OBJECT_0) {
                fprintf(stderr, "[*] proceso cliente PID %lu termino\n", g_trackedPids[i]);
                CloseHandle(g_trackedHandles[i]);
                g_trackedCount--;
                g_trackedHandles[i] = g_trackedHandles[g_trackedCount];
                g_trackedPids[i] = g_trackedPids[g_trackedCount];
            } else {
                i++;
            }
        }
        alive = g_trackedCount;
        LeaveCriticalSection(&g_trackLock);

        if (alive > 0 || g_activeConns > 0) {
            idleSince = 0;
            continue;
        }

        if (!g_everConnected) {
            if (now - start >= STARTUP_GRACE_MS) {
                fprintf(stderr, "[*] ninguna app se conecto en %d s, saliendo\n", STARTUP_GRACE_MS / 1000);
                ExitProcess(0);
            }
            continue;
        }

        if (idleSince == 0) {
            idleSince = now;
        } else if (now - idleSince >= IDLE_GRACE_MS) {
            fprintf(stderr, "[*] todas las apps cliente se cerraron, saliendo\n");
            ExitProcess(0);
        }
    }
    return 0;
}

typedef struct {
    SOCKET client;
} ClientArgs;

static DWORD WINAPI client_thread(LPVOID param)
{
    ClientArgs *ca = (ClientArgs *)param;
    InterlockedIncrement(&g_activeConns);
    InterlockedExchange(&g_everConnected, 1);
    track_client_process(ca->client);
    handle_client(ca->client);
    InterlockedDecrement(&g_activeConns);
    free(ca);
    return 0;
}

/* Sin consola, stderr no va a ningun lado: lo mandamos a un log que se
   reinicia en cada arranque, para poder diagnosticar (ej. un 502). */
static void open_log(void)
{
    const char *base = getenv("LOCALAPPDATA");
    char dir[MAX_PATH], path[MAX_PATH];

    if (!base) return;
    _snprintf(dir, sizeof(dir), "%s\\ClaudeVPNProxy", base);
    dir[sizeof(dir) - 1] = '\0';
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof(path), "%s\\claude_proxy.log", dir);
    path[sizeof(path) - 1] = '\0';
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in addr;
    int port;

    if (argc != 3) {
        fprintf(stderr, "uso: %s <puerto_local> <nombre_adaptador>\n", argv[0]);
        return 1;
    }

    open_log();
    InitializeCriticalSection(&g_trackLock);

    port = atoi(argv[1]);
    g_listenPortNet = htons((u_short)port);
    strncpy(g_adapterName, argv[2], sizeof(g_adapterName) - 1);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[!] WSAStartup fallo\n");
        return 1;
    }

    if (!resolve_adapter_index(g_adapterName, &g_boundIfIndex)) {
        fprintf(stderr, "[!] No se encontro el adaptador '%s'. "
                        "Verifica que el tunel WireGuard este activo (el nombre debe "
                        "coincidir exactamente con el FriendlyName de la interfaz).\n",
                g_adapterName);
        WSACleanup();
        return 1;
    }
    fprintf(stderr, "[*] Adaptador '%s' -> IfIndex %lu\n", g_adapterName, g_boundIfIndex);

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        fprintf(stderr, "[!] socket() fallo\n");
        WSACleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* solo localhost, nunca expuesto a la red */
    addr.sin_port = htons((u_short)port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[!] bind() fallo en 127.0.0.1:%d (%d)\n", port, WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    if (listen(listener, LISTEN_BACKLOG) != 0) {
        fprintf(stderr, "[!] listen() fallo\n");
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    fprintf(stderr, "[*] claude_proxy escuchando en 127.0.0.1:%d, saliendo por '%s'\n", port, g_adapterName);

    {
        HANDLE hm = CreateThread(NULL, 0, monitor_thread, NULL, 0, NULL);
        if (hm) CloseHandle(hm);
    }

    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        ClientArgs *ca;
        HANDLE h;

        if (client == INVALID_SOCKET)
            continue;

        ca = (ClientArgs *)malloc(sizeof(ClientArgs));
        ca->client = client;
        h = CreateThread(NULL, 0, client_thread, ca, 0, NULL);
        if (h) CloseHandle(h);
    }

    closesocket(listener);
    WSACleanup();
    return 0;
}
