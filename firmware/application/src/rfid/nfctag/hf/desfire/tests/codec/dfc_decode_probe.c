/* dfc_decode_probe — decode a .dfcb with the real engine, report the status.
 *
 * Bridges the Python credential codec (chameleon_dfc.py) to the firmware's own
 * DER decoder so a host test can prove the bytes the CLI sends are accepted by
 * the engine the device runs. Exit code is the DfcDerStatus (0 = Ok).
 *
 *   cc dfc_decode_probe.c ../../src/dfc_*.c <host+crypto> -o probe
 *   ./probe card.dfcb   # prints "Ok"/"Malformed"/... ; exit == status
 */
#include "dfc_credential.h"
#include "dfc_der.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DfcCredential cred;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.dfcb>\n", argv[0]); return 99; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 98; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); return 97; }
    fclose(f);

    memset(&cred, 0, sizeof(cred));
    DfcDerStatus st = dfc_der_decode(&cred, buf, n);
    const char *name = st == DfcDerOk ? "Ok"
                       : st == DfcDerMalformed ? "Malformed"
                       : st == DfcDerUnsupported ? "Unsupported"
                       : st == DfcDerCapacity ? "Capacity" : "Unknown";
    printf("%s\n", name);
    free(buf);
    return (int)st;
}
