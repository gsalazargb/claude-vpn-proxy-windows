/*
 * prep_conf.c
 *
 * Copia un archivo .conf de WireGuard a destino, inyectando "Table = off"
 * justo despues de la linea "[Interface]" si no existe ya una linea Table=
 * en esa seccion. Asi el instalador NSIS no tiene que parsear texto a mano.
 *
 * Uso: prep_conf.exe <origen.conf> <destino.conf>
 *
 * Exit code 0 = ok, 1 = error de uso/archivo, 2 = el .conf no tiene
 * seccion [Interface] (no parece un .conf de WireGuard valido), 3 = la
 * PrivateKey viene enmascarada o invalida (pasa esto si alguien re-descarga
 * un .conf ya generado antes en vez de uno nuevo: algunos paneles de VPN
 * solo muestran la clave privada completa una vez, al crearla).
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Una clave privada WireGuard es un valor de 32 bytes en base64, siempre
   44 caracteres incluyendo el '=' de relleno final. No validamos el
   contenido criptografico, solo que no sea un placeholder obvio. */
static int looks_like_valid_wg_key(const char *key)
{
    size_t len = strlen(key);
    size_t i;

    if (len != 44) return 0;
    if (key[len - 1] != '=') return 0;
    if (key[0] == '*') return 0;

    for (i = 0; i < len - 1; i++) {
        char c = key[i];
        if (!isalnum((unsigned char)c) && c != '+' && c != '/')
            return 0;
    }
    return 1;
}

static void extract_value(const char *line, char *out, size_t outsize)
{
    const char *eq = strchr(line, '=');
    size_t i = 0, j = 0;

    out[0] = '\0';
    if (!eq) return;
    eq++;
    while (*eq && isspace((unsigned char)*eq)) eq++;
    while (eq[i] && j < outsize - 1) {
        char c = eq[i++];
        if (c == '\r' || c == '\n') break;
        out[j++] = c;
    }
    while (j > 0 && isspace((unsigned char)out[j - 1])) j--;
    out[j] = '\0';
}

static void trim_lower_copy(const char *src, char *dst, size_t dstsize)
{
    size_t i = 0, j = 0;
    while (src[i] && isspace((unsigned char)src[i])) i++;
    for (; src[i] && j < dstsize - 1; i++) {
        dst[j++] = (char)tolower((unsigned char)src[i]);
    }
    while (j > 0 && isspace((unsigned char)dst[j - 1])) j--;
    dst[j] = '\0';
}

int main(int argc, char **argv)
{
    FILE *in, *out;
    char line[2048];
    char lower[2048];
    int inInterfaceSection = 0;
    int sawInterfaceSection = 0;
    int sawTableInInterface = 0;
    int sawPrivateKey = 0;
    int injected = 0;
    char privateKey[256];

    if (argc != 3) {
        fprintf(stderr, "uso: %s <origen.conf> <destino.conf>\n", argv[0]);
        return 1;
    }

    in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "[!] no se pudo abrir '%s'\n", argv[1]);
        return 1;
    }

    /* Primero: ver si la seccion [Interface] ya trae una linea Table=,
       y validar que la PrivateKey no venga enmascarada */
    while (fgets(line, sizeof(line), in)) {
        trim_lower_copy(line, lower, sizeof(lower));
        if (lower[0] == '[') {
            inInterfaceSection = (strcmp(lower, "[interface]") == 0);
            if (inInterfaceSection) sawInterfaceSection = 1;
            continue;
        }
        if (inInterfaceSection && strncmp(lower, "table", 5) == 0) {
            sawTableInInterface = 1;
        }
        if (inInterfaceSection && strncmp(lower, "privatekey", 10) == 0) {
            extract_value(line, privateKey, sizeof(privateKey));
            sawPrivateKey = 1;
        }
    }

    if (!sawInterfaceSection) {
        fprintf(stderr, "[!] '%s' no tiene seccion [Interface], no parece un .conf de WireGuard valido\n", argv[1]);
        fclose(in);
        return 2;
    }

    if (!sawPrivateKey || !looks_like_valid_wg_key(privateKey)) {
        fprintf(stderr, "[!] La PrivateKey de '%s' parece enmascarada o invalida.\n"
                        "    Esto pasa si re-descargas un .conf ya generado antes: muchos paneles de\n"
                        "    VPN (Proton incluido) solo muestran la clave privada completa la primera\n"
                        "    vez. Genera un perfil/.conf NUEVO y usa ese.\n", argv[1]);
        fclose(in);
        return 3;
    }

    rewind(in);

    out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "[!] no se pudo escribir '%s'\n", argv[2]);
        fclose(in);
        return 1;
    }

    inInterfaceSection = 0;
    while (fgets(line, sizeof(line), in)) {
        trim_lower_copy(line, lower, sizeof(lower));

        if (lower[0] == '[') {
            inInterfaceSection = (strcmp(lower, "[interface]") == 0);
            fputs(line, out);
            if (inInterfaceSection && !sawTableInInterface && !injected) {
                fputs("Table = off\r\n", out);
                injected = 1;
            }
            continue;
        }

        fputs(line, out);
    }

    fclose(in);
    fclose(out);

    fprintf(stderr, "[*] '%s' -> '%s' (Table=off %s)\n", argv[1], argv[2],
            injected ? "inyectado" : "ya estaba presente");
    return 0;
}
