#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#include "protocolo.h"

static int  sockfd   = -1;
static char my_username[32];
static volatile int corriendo = 1;



static void send_packet(int fd, ChatPacket *pkt) {
    size_t total   = sizeof(ChatPacket);
    size_t enviado = 0;
    char  *ptr     = (char *)pkt;

    while (enviado < total) {
        ssize_t n = send(fd, ptr + enviado, total - enviado, 0);
        if (n <= 0) break;
        enviado += (size_t)n;
    }
}

static void print_prompt(void) {
    printf("[%s]> ", my_username);
    fflush(stdout);
}


static void *hilo_receptor(void *arg) {
    (void)arg;
    ChatPacket pkt;

    while (corriendo) {
        memset(&pkt, 0, sizeof(pkt));
        int n = recv(sockfd, &pkt, sizeof(pkt), MSG_WAITALL);
        if (n <= 0) {
            if (corriendo) {
                printf("\n[!] Conexión con el servidor perdida.\n");
            }
            corriendo = 0;
            break;
        }

        pkt.sender[31]   = '\0';
        pkt.target[31]   = '\0';
        pkt.payload[956] = '\0';

        printf("\r");

        switch (pkt.command) {

            case CMD_OK:
                printf("[OK] %s\n", pkt.payload[0] ? pkt.payload : "Operación exitosa");
                break;

            case CMD_ERROR:
                printf("[ERROR] %s\n", pkt.payload);
                break;

            case CMD_MSG:
                if (strcmp(pkt.sender, "SERVER") == 0) {
                    printf("[SERVER] %s\n", pkt.payload);
                } else if (strcmp(pkt.target, "ALL") == 0) {
                    printf("[BROADCAST] %s: %s\n", pkt.sender, pkt.payload);
                } else {
                    printf("[DM de %s] %s\n", pkt.sender, pkt.payload);
                }
                break;

            case CMD_USER_LIST:
                printf("┌─ Usuarios conectados ─────────────────────\n");
                {
                    char buf[sizeof(pkt.payload)];
                    strncpy(buf, pkt.payload, sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';

                    char *entry = strtok(buf, ";");
                    while (entry) {
                        char *comma = strchr(entry, ',');
                        if (comma) {
                            *comma = '\0';
                            printf("│  %-20s %s\n", entry, comma + 1);
                        } else {
                            printf("│  %s\n", entry);
                        }
                        entry = strtok(NULL, ";");
                    }
                }
                printf("└───────────────────────────────────────────\n");
                break;

            case CMD_USER_INFO:
                {
                    char buf[sizeof(pkt.payload)];
                    strncpy(buf, pkt.payload, sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';

                    char *comma = strchr(buf, ',');
                    if (comma) {
                        *comma = '\0';
                        printf("[INFO] IP: %s  |  Status: %s\n", buf, comma + 1);
                    } else {
                        printf("[INFO] %s\n", buf);
                    }
                }
                break;

            case CMD_DISCONNECTED:
                printf("[--] %s se ha desconectado.\n", pkt.payload);
                break;

            default:
                break;
        }

        print_prompt();
    }

    return NULL;
}

/* Ayuda  */
static void print_help(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║              Comandos disponibles                    ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  /broadcast <mensaje>         Chat general           ║\n");
    printf("║  /msg <usuario> <mensaje>     Mensaje privado        ║\n");
    printf("║  /status <ACTIVE|BUSY|INACTIVE> Cambiar status       ║\n");
    printf("║  /list                        Listar usuarios        ║\n");
    printf("║  /info <usuario>              Info de un usuario     ║\n");
    printf("║  /help                        Mostrar esta ayuda     ║\n");
    printf("║  /exit                        Salir del chat         ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
}

/* Main */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <username> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    const char *username   = argv[1];
    const char *ip_srv     = argv[2];
    int         puerto     = atoi(argv[3]);

    if (strlen(username) == 0 || strlen(username) >= 32) {
        fprintf(stderr, "Username debe tener entre 1 y 31 caracteres.\n");
        return 1;
    }
    strncpy(my_username, username, sizeof(my_username) - 1);

    /* socket TCP */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    /* Conectar al servidor */
    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port   = htons((uint16_t)puerto);

    if (inet_pton(AF_INET, ip_srv, &srv_addr.sin_addr) <= 0) {
        fprintf(stderr, "IP inválida: %s\n", ip_srv);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("connect");
        return 1;
    }

    printf("Conectado al servidor %s:%d\n", ip_srv, puerto);

    /* Enviar CMD_REGISTER */
    {
        ChatPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.command = CMD_REGISTER;
        strncpy(pkt.sender,  my_username, sizeof(pkt.sender)  - 1);
        strncpy(pkt.payload, my_username, sizeof(pkt.payload) - 1);
        pkt.payload_len = (uint16_t)strlen(pkt.payload);
        send_packet(sockfd, &pkt);
    }

    /* Esperar respuesta del registro */
    {
        ChatPacket resp;
        memset(&resp, 0, sizeof(resp));
        int n = recv(sockfd, &resp, sizeof(resp), MSG_WAITALL);
        if (n <= 0) {
            fprintf(stderr, "Servidor cerró la conexión durante el registro.\n");
            close(sockfd);
            return 1;
        }
        resp.payload[956] = '\0';
        if (resp.command == CMD_ERROR) {
            fprintf(stderr, "Registro rechazado: %s\n", resp.payload);
            close(sockfd);
            return 1;
        }
        
        printf("[OK] %s\n", resp.payload);
    }

    /* Lanzar hilo receptor */
    pthread_t tid_rx;
    if (pthread_create(&tid_rx, NULL, hilo_receptor, NULL) != 0) {
        perror("pthread_create");
        close(sockfd);
        return 1;
    }
    pthread_detach(tid_rx);

    /* Mostrar ayuda inicial */
    print_help();
    print_prompt();

    /* Loop principal: leer comandos del usuario */
    char linea[1024];
    while (corriendo && fgets(linea, sizeof(linea), stdin)) {

        linea[strcspn(linea, "\n")] = '\0';

        if (linea[0] == '\0') {
            print_prompt();
            continue;
        }

        /* Parsear y enviar comando */

        if (strncmp(linea, "/broadcast ", 11) == 0) {
            const char *mensaje = linea + 11;
            if (*mensaje == '\0') {
                printf("[!] Uso: /broadcast <mensaje>\n");
            } else {
                ChatPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.command = CMD_BROADCAST;
                strncpy(pkt.sender,  my_username, sizeof(pkt.sender)  - 1);
                strncpy(pkt.payload, mensaje,     sizeof(pkt.payload) - 1);
                pkt.payload_len = (uint16_t)strlen(pkt.payload);
                send_packet(sockfd, &pkt);
            }

        } else if (strncmp(linea, "/msg ", 5) == 0) {
            /* /msg <usuario> <mensaje> */
            char *resto    = linea + 5;
            char *espacio  = strchr(resto, ' ');
            if (!espacio || *(espacio + 1) == '\0') {
                printf("[!] Uso: /msg <usuario> <mensaje>\n");
            } else {
                *espacio = '\0';
                const char *dest    = resto;
                const char *mensaje = espacio + 1;

                ChatPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.command = CMD_DIRECT;
                strncpy(pkt.sender,  my_username, sizeof(pkt.sender)  - 1);
                strncpy(pkt.target,  dest,        sizeof(pkt.target)  - 1);
                strncpy(pkt.payload, mensaje,     sizeof(pkt.payload) - 1);
                pkt.payload_len = (uint16_t)strlen(pkt.payload);
                send_packet(sockfd, &pkt);
            }

        } else if (strncmp(linea, "/status ", 8) == 0) {
            /* /status <ACTIVE|BUSY|INACTIVE> */
            const char *nuevo = linea + 8;
            if (strcmp(nuevo, STATUS_ACTIVE)   != 0 &&
                strcmp(nuevo, STATUS_BUSY)     != 0 &&
                strcmp(nuevo, STATUS_INACTIVE) != 0) {
                printf("[!] Status válidos: ACTIVE, BUSY, INACTIVE\n");
            } else {
                ChatPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.command = CMD_STATUS;
                strncpy(pkt.sender,  my_username, sizeof(pkt.sender)  - 1);
                strncpy(pkt.payload, nuevo,       sizeof(pkt.payload) - 1);
                pkt.payload_len = (uint16_t)strlen(pkt.payload);
                send_packet(sockfd, &pkt);
            }

        } else if (strcmp(linea, "/list") == 0) {
            /* /list */
            ChatPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.command = CMD_LIST;
            strncpy(pkt.sender, my_username, sizeof(pkt.sender) - 1);
            pkt.payload_len = 0;
            send_packet(sockfd, &pkt);

        } else if (strncmp(linea, "/info ", 6) == 0) {
            /* /info <usuario> */
            const char *target = linea + 6;
            if (*target == '\0') {
                printf("[!] Uso: /info <usuario>\n");
            } else {
                ChatPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.command = CMD_INFO;
                strncpy(pkt.sender, my_username, sizeof(pkt.sender) - 1);
                strncpy(pkt.target, target,      sizeof(pkt.target) - 1);
                pkt.payload_len = 0;
                send_packet(sockfd, &pkt);
            }

        } else if (strcmp(linea, "/help") == 0) {
            print_help();

        } else if (strcmp(linea, "/exit") == 0) {
            /* Enviar CMD_LOGOUT y esperar CMD_OK antes de cerrar */
            ChatPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.command = CMD_LOGOUT;
            strncpy(pkt.sender, my_username, sizeof(pkt.sender) - 1);
            pkt.payload_len = 0;
            send_packet(sockfd, &pkt);

            /* Dar un momento para que el hilo receptor imprima el CMD_OK */
            usleep(200000);  /* 200 ms */
            corriendo = 0;
            break;

        } else {
            printf("[!] Comando no reconocido. Escribe /help para ver los comandos.\n");
        }

        if (corriendo) print_prompt();
    }

    /* Limpieza */
    corriendo = 0;
    close(sockfd);
    printf("\nHasta luego, %s!\n", my_username);
    return 0;
}
