#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>

#include "protocolo.h"

#define MAX_CLIENTES 100

typedef struct {
    char    username[32];
    char    ip[INET_ADDRSTRLEN];  /* ej: "34.201.55.12" */
    char    status[16];           /* STATUS_ACTIVE | STATUS_BUSY | STATUS_INACTIVE */
    int     sockfd;
    int     activo;               /* 1=conectado 0=libre */
    time_t  ultimo_mensaje;       /* actualizar en cada recv — para detectar inactividad */
} Cliente;

Cliente         lista[MAX_CLIENTES];
int             num_clientes = 0;
pthread_mutex_t mutex_lista  = PTHREAD_MUTEX_INITIALIZER;

/* ── Prototipos ─────────────────────────────────────────────── */
int   init_server(int puerto);
void *hilo_cliente(void *arg);
void  handle_register(int idx, ChatPacket *pkt, struct sockaddr_in *addr);
void  handle_broadcast(int idx, ChatPacket *pkt);
void  handle_direct(int idx, ChatPacket *pkt);
void  handle_list(int idx, ChatPacket *pkt);
void  handle_info(int idx, ChatPacket *pkt);
void  handle_status(int idx, ChatPacket *pkt);
void  handle_logout(int idx);
void  remove_client(int idx);
void  broadcast_all(ChatPacket *pkt, int except_idx);
void  send_packet(int sockfd, ChatPacket *pkt);
int   find_by_name(const char *username);
void  check_inactivity(void);

/* ═══════════════════════════════════════════════════════════════
   1) send_packet — envía exactamente sizeof(ChatPacket) bytes
   ═══════════════════════════════════════════════════════════════
   send() puede enviar menos bytes de los pedidos (short write),
   por lo que usamos un loop que avanza el puntero hasta
   completar los 1024 bytes del paquete.                        */
void send_packet(int sockfd, ChatPacket *pkt) {
    size_t total   = sizeof(ChatPacket);  /* 1024 */
    size_t enviado = 0;
    char  *ptr     = (char *)pkt;

    while (enviado < total) {
        ssize_t n = send(sockfd, ptr + enviado, total - enviado, 0);
        if (n <= 0) break;   /* error o desconexión — no podemos hacer más */
        enviado += (size_t)n;
    }
}

/* ═══════════════════════════════════════════════════════════════
   2) find_by_name — busca username entre clientes activos
   ═══════════════════════════════════════════════════════════════
   Adquiere mutex porque lista[] se comparte entre hilos.
   Retorna el índice del cliente o -1 si no se encuentra.       */
int find_by_name(const char *username) {
    int resultado = -1;

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            resultado = i;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    return resultado;
}

/* ═══════════════════════════════════════════════════════════════
   3) broadcast_all — envía pkt a todos los clientes activos
   ═══════════════════════════════════════════════════════════════
   except_idx = índice a excluir  (-1 = enviar a todos).

   ¿Por qué NO hacemos send() dentro del mutex?
   Porque send() puede bloquearse si el buffer TCP del receptor
   está lleno. Si mantuviéramos el mutex durante el send():
     • Todos los demás hilos quedarían bloqueados esperando
       el mutex (no podrían recibir, registrar ni salir).
     • En el peor caso se produce un deadlock si dos hilos
       intentan enviarse mutuamente con el buffer lleno.
   La solución: copiamos los fd y el conteo bajo mutex,
   y luego enviamos fuera del mutex.                            */
void broadcast_all(ChatPacket *pkt, int except_idx) {
    int fds[MAX_CLIENTES];
    int count = 0;

    /* ── Sección crítica: solo lectura de lista[] ── */
    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && i != except_idx) {
            fds[count++] = lista[i].sockfd;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    /* ── Envío fuera del mutex ── */
    for (int i = 0; i < count; i++) {
        send_packet(fds[i], pkt);
    }
}

/* ═══════════════════════════════════════════════════════════════
   4) remove_client — desconecta y notifica a todos
   ═══════════════════════════════════════════════════════════════
   1. Guarda el username antes de limpiar el slot.
   2. Marca activo=0 y cierra el socket (bajo mutex).
   3. Arma CMD_DISCONNECTED y hace broadcast fuera del mutex.   */
void remove_client(int idx) {
    char username_copia[32];

    /* ── Sección crítica ── */
    pthread_mutex_lock(&mutex_lista);
    strncpy(username_copia, lista[idx].username, sizeof(username_copia) - 1);
    username_copia[sizeof(username_copia) - 1] = '\0';
    lista[idx].activo = 0;
    close(lista[idx].sockfd);
    pthread_mutex_unlock(&mutex_lista);

    printf("Cliente desconectado: %s\n", username_copia);

    /* ── Broadcast fuera del mutex ── */
    ChatPacket aviso;
    memset(&aviso, 0, sizeof(aviso));
    aviso.command = CMD_DISCONNECTED;
    strncpy(aviso.sender, "SERVER", sizeof(aviso.sender) - 1);
    strncpy(aviso.target, "ALL",    sizeof(aviso.target) - 1);
    strncpy(aviso.payload, username_copia, sizeof(aviso.payload) - 1);
    aviso.payload_len = (uint16_t)strlen(aviso.payload);

    broadcast_all(&aviso, idx);
}

/* ═══════════════════════════════════════════════════════════════
   Stubs restantes — TODO por implementar
   ═══════════════════════════════════════════════════════════════ */

int init_server(int puerto) {
    /* 1. Crear socket TCP */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    /* 2. Permitir reusar el puerto inmediatamente tras cerrar */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt"); exit(1);
    }

    /* 3. Bind a INADDR_ANY:puerto */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)puerto);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    /* 4. Listen con backlog de 10 conexiones pendientes */
    if (listen(fd, 10) < 0) {
        perror("listen"); exit(1);
    }

    printf("Servidor escuchando en puerto %d\n", puerto);
    return fd;
}

/* ═══════════════════════════════════════════════════════════════
   hilo_cliente — un hilo por conexión
   ═══════════════════════════════════════════════════════════════ */
void *hilo_cliente(void *arg) {
    int idx = *(int *)arg;
    free(arg);
    ChatPacket pkt;
    int registrado = 0;

    /* Recuperar la dirección del cliente (guardada en main con inet_ntop,
       pero necesitamos el struct para handle_register) */
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(lista[idx].sockfd, (struct sockaddr *)&addr, &addr_len);

    while (1) {
        /* Revisar inactividad en cada iteración */
        check_inactivity();

        /* Leer exactamente 1024 bytes (tamaño de ChatPacket) */
        memset(&pkt, 0, sizeof(pkt));
        int n = recv(lista[idx].sockfd, &pkt, sizeof(pkt), MSG_WAITALL);
        if (n <= 0) {
            /* Desconexión abrupta o error */
            if (registrado) remove_client(idx);
            else { close(lista[idx].sockfd); lista[idx].activo = 0; }
            return NULL;
        }

        /* Forzar null-terminators contra clientes maliciosos */
        pkt.payload[956] = '\0';
        pkt.sender[31]   = '\0';
        pkt.target[31]   = '\0';

        /* Actualizar timestamp de último mensaje */
        pthread_mutex_lock(&mutex_lista);
        lista[idx].ultimo_mensaje = time(NULL);
        /* Si estaba INACTIVE y envía algo, volver a ACTIVE */
        if (registrado && strcmp(lista[idx].status, STATUS_INACTIVE) == 0) {
            strncpy(lista[idx].status, STATUS_ACTIVE, sizeof(lista[idx].status) - 1);
        }
        pthread_mutex_unlock(&mutex_lista);

        /* ── Fase de registro: solo aceptar CMD_REGISTER ── */
        if (!registrado) {
            if (pkt.command == CMD_REGISTER) {
                handle_register(idx, &pkt, &addr);
                /* Si handle_register falló, activo ya es 0 */
                if (!lista[idx].activo) return NULL;
                registrado = 1;
            }
            /* Ignorar cualquier otro comando hasta estar registrado */
            continue;
        }

        /* ── Despacho de comandos ── */
        switch (pkt.command) {
            case CMD_BROADCAST: handle_broadcast(idx, &pkt); break;
            case CMD_DIRECT:    handle_direct(idx, &pkt);    break;
            case CMD_LIST:      handle_list(idx, &pkt);      break;
            case CMD_INFO:      handle_info(idx, &pkt);      break;
            case CMD_STATUS:    handle_status(idx, &pkt);    break;
            case CMD_LOGOUT:    handle_logout(idx); return NULL;
            default: break; /* Ignorar comandos desconocidos sin crashear */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   handle_register — CMD_REGISTER
   ═══════════════════════════════════════════════════════════════ */
void handle_register(int idx, ChatPacket *pkt, struct sockaddr_in *addr) {
    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    strncpy(resp.target, pkt->sender, sizeof(resp.target) - 1);

    /* Obtener IP del cliente */
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));

    pthread_mutex_lock(&mutex_lista);

    /* Verificar username duplicado */
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && i != idx &&
            strcmp(lista[i].username, pkt->sender) == 0) {
            pthread_mutex_unlock(&mutex_lista);
            resp.command = CMD_ERROR;
            strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
            strncpy(resp.payload, "Usuario ya existe", sizeof(resp.payload) - 1);
            resp.payload_len = (uint16_t)strlen(resp.payload);
            send_packet(lista[idx].sockfd, &resp);
            close(lista[idx].sockfd);
            lista[idx].activo = 0;
            return;
        }
    }

    /* Verificar IP duplicada */
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && i != idx &&
            strcmp(lista[i].ip, ip_str) == 0) {
            pthread_mutex_unlock(&mutex_lista);
            resp.command = CMD_ERROR;
            strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
            strncpy(resp.payload, "IP ya conectada", sizeof(resp.payload) - 1);
            resp.payload_len = (uint16_t)strlen(resp.payload);
            send_packet(lista[idx].sockfd, &resp);
            close(lista[idx].sockfd);
            lista[idx].activo = 0;
            return;
        }
    }

    /* Registro exitoso: llenar datos del cliente */
    strncpy(lista[idx].username, pkt->sender, sizeof(lista[idx].username) - 1);
    strncpy(lista[idx].ip, ip_str, sizeof(lista[idx].ip) - 1);
    strncpy(lista[idx].status, STATUS_ACTIVE, sizeof(lista[idx].status) - 1);
    lista[idx].ultimo_mensaje = time(NULL);

    pthread_mutex_unlock(&mutex_lista);

    /* Responder CMD_OK */
    resp.command = CMD_OK;
    strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
    snprintf(resp.payload, sizeof(resp.payload), "Bienvenido %s", pkt->sender);
    resp.payload_len = (uint16_t)strlen(resp.payload);
    send_packet(lista[idx].sockfd, &resp);

    printf("Usuario registrado: %s (%s)\n", pkt->sender, ip_str);
}

/* ═══════════════════════════════════════════════════════════════
   handle_broadcast — CMD_BROADCAST
   ═══════════════════════════════════════════════════════════════ */
void handle_broadcast(int idx, ChatPacket *pkt) {
    ChatPacket msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = CMD_MSG;
    strncpy(msg.sender, lista[idx].username, sizeof(msg.sender) - 1);
    strncpy(msg.target, "ALL", sizeof(msg.target) - 1);
    strncpy(msg.payload, pkt->payload, sizeof(msg.payload) - 1);
    msg.payload_len = (uint16_t)strlen(msg.payload);

    /* -1 = enviar a TODOS, incluido el emisor */
    broadcast_all(&msg, -1);
}

/* ═══════════════════════════════════════════════════════════════
   handle_direct — CMD_DIRECT
   ═══════════════════════════════════════════════════════════════ */
void handle_direct(int idx, ChatPacket *pkt) {
    int dest = find_by_name(pkt->target);

    if (dest == -1) {
        /* Destinatario no encontrado */
        ChatPacket err;
        memset(&err, 0, sizeof(err));
        err.command = CMD_ERROR;
        strncpy(err.sender, "SERVER", sizeof(err.sender) - 1);
        strncpy(err.target, lista[idx].username, sizeof(err.target) - 1);
        strncpy(err.payload, "Destinatario no conectado", sizeof(err.payload) - 1);
        err.payload_len = (uint16_t)strlen(err.payload);
        send_packet(lista[idx].sockfd, &err);
        return;
    }

    /* Enviar CMD_MSG solo al destinatario */
    ChatPacket msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = CMD_MSG;
    strncpy(msg.sender, lista[idx].username, sizeof(msg.sender) - 1);
    strncpy(msg.target, pkt->target, sizeof(msg.target) - 1);
    strncpy(msg.payload, pkt->payload, sizeof(msg.payload) - 1);
    msg.payload_len = (uint16_t)strlen(msg.payload);
    send_packet(lista[dest].sockfd, &msg);
}

/* ═══════════════════════════════════════════════════════════════
   handle_list — CMD_LIST
   ═══════════════════════════════════════════════════════════════ */
void handle_list(int idx, ChatPacket *pkt) {
    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.command = CMD_USER_LIST;
    strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
    strncpy(resp.target, lista[idx].username, sizeof(resp.target) - 1);

    /* Construir payload: "alice,ACTIVE;bob,BUSY;..." */
    char buffer[sizeof(resp.payload)];
    buffer[0] = '\0';
    int offset = 0;

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo) {
            int written = snprintf(buffer + offset, sizeof(buffer) - offset,
                                   "%s%s,%s",
                                   (offset > 0) ? ";" : "",
                                   lista[i].username, lista[i].status);
            if (written > 0 && offset + written < (int)sizeof(buffer)) {
                offset += written;
            }
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    strncpy(resp.payload, buffer, sizeof(resp.payload) - 1);
    resp.payload_len = (uint16_t)strlen(resp.payload);
    send_packet(lista[idx].sockfd, &resp);
}

/* ═══════════════════════════════════════════════════════════════
   handle_info — CMD_INFO
   ═══════════════════════════════════════════════════════════════ */
void handle_info(int idx, ChatPacket *pkt) {
    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
    strncpy(resp.target, lista[idx].username, sizeof(resp.target) - 1);

    int target_idx = find_by_name(pkt->target);

    if (target_idx == -1) {
        resp.command = CMD_ERROR;
        strncpy(resp.payload, "Usuario no conectado", sizeof(resp.payload) - 1);
    } else {
        resp.command = CMD_USER_INFO;
        pthread_mutex_lock(&mutex_lista);
        snprintf(resp.payload, sizeof(resp.payload), "%s,%s",
                 lista[target_idx].ip, lista[target_idx].status);
        pthread_mutex_unlock(&mutex_lista);
    }

    resp.payload_len = (uint16_t)strlen(resp.payload);
    send_packet(lista[idx].sockfd, &resp);
}

/* ═══════════════════════════════════════════════════════════════
   handle_status — CMD_STATUS
   ═══════════════════════════════════════════════════════════════ */
void handle_status(int idx, ChatPacket *pkt) {
    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
    strncpy(resp.target, lista[idx].username, sizeof(resp.target) - 1);

    /* Validar que sea un valor permitido */
    if (strcmp(pkt->payload, STATUS_ACTIVE)   != 0 &&
        strcmp(pkt->payload, STATUS_BUSY)     != 0 &&
        strcmp(pkt->payload, STATUS_INACTIVE) != 0) {
        resp.command = CMD_ERROR;
        strncpy(resp.payload, "Status inválido", sizeof(resp.payload) - 1);
        resp.payload_len = (uint16_t)strlen(resp.payload);
        send_packet(lista[idx].sockfd, &resp);
        return;
    }

    /* Actualizar status en lista[] */
    pthread_mutex_lock(&mutex_lista);
    strncpy(lista[idx].status, pkt->payload, sizeof(lista[idx].status) - 1);
    lista[idx].status[sizeof(lista[idx].status) - 1] = '\0';
    pthread_mutex_unlock(&mutex_lista);

    /* Confirmar al cliente */
    resp.command = CMD_OK;
    strncpy(resp.payload, pkt->payload, sizeof(resp.payload) - 1);
    resp.payload_len = (uint16_t)strlen(resp.payload);
    send_packet(lista[idx].sockfd, &resp);
}

/* ═══════════════════════════════════════════════════════════════
   handle_logout — CMD_LOGOUT
   ═══════════════════════════════════════════════════════════════ */
void handle_logout(int idx) {
    /* Enviar CMD_OK al cliente antes de desconectarlo */
    ChatPacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.command = CMD_OK;
    strncpy(resp.sender, "SERVER", sizeof(resp.sender) - 1);
    strncpy(resp.target, lista[idx].username, sizeof(resp.target) - 1);
    resp.payload_len = 0;
    send_packet(lista[idx].sockfd, &resp);

    /* Eliminar y notificar a los demás */
    remove_client(idx);
}

/* ═══════════════════════════════════════════════════════════════
   check_inactivity — detecta clientes inactivos
   ═══════════════════════════════════════════════════════════════
   Recorre lista[] buscando clientes activos cuyo último mensaje
   sea mayor a INACTIVITY_TIMEOUT. Les cambia el status a INACTIVE
   y les notifica con un CMD_MSG.                                */
void check_inactivity(void) {
    time_t ahora = time(NULL);

    /* Recopilar los que necesitan cambio bajo mutex */
    int inactivos[MAX_CLIENTES];
    int fds[MAX_CLIENTES];
    int count = 0;

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo &&
            strcmp(lista[i].status, STATUS_INACTIVE) != 0 &&
            (ahora - lista[i].ultimo_mensaje) > INACTIVITY_TIMEOUT) {
            strncpy(lista[i].status, STATUS_INACTIVE, sizeof(lista[i].status) - 1);
            inactivos[count] = i;
            fds[count] = lista[i].sockfd;
            count++;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    /* Notificar fuera del mutex */
    for (int i = 0; i < count; i++) {
        ChatPacket aviso;
        memset(&aviso, 0, sizeof(aviso));
        aviso.command = CMD_MSG;
        strncpy(aviso.sender, "SERVER", sizeof(aviso.sender) - 1);
        strncpy(aviso.target, lista[inactivos[i]].username, sizeof(aviso.target) - 1);
        strncpy(aviso.payload, "Tu status cambió a INACTIVE", sizeof(aviso.payload) - 1);
        aviso.payload_len = (uint16_t)strlen(aviso.payload);
        send_packet(fds[i], &aviso);

        printf("Inactividad: %s → INACTIVE\n", lista[inactivos[i]].username);
    }
}

int main(int argc, char *argv[]) {
    /* Validar argumentos */
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1;
    }

    int server_fd = init_server(atoi(argv[1]));

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        /* Buscar slot libre en lista[] */
        pthread_mutex_lock(&mutex_lista);
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (!lista[i].activo) {
                idx = i;
                lista[i].sockfd = client_fd;
                lista[i].activo = 1;
                /* Guardar IP del cliente */
                inet_ntop(AF_INET, &client_addr.sin_addr,
                          lista[i].ip, sizeof(lista[i].ip));
                break;
            }
        }
        pthread_mutex_unlock(&mutex_lista);

        if (idx == -1) {
            fprintf(stderr, "Servidor lleno, rechazando conexión\n");
            close(client_fd);
            continue;
        }

        /* Crear hilo para el nuevo cliente */
        int *p_idx = malloc(sizeof(int));
        if (!p_idx) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        *p_idx = idx;

        pthread_t tid;
        if (pthread_create(&tid, NULL, hilo_cliente, p_idx) != 0) {
            perror("pthread_create");
            free(p_idx);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);

        printf("Nueva conexión desde %s (slot %d)\n", lista[idx].ip, idx);
    }

    return 0;
}
