# Chat Cliente-Servidor en C (TCP)

Proyecto de chat concurrente desarrollado en C usando sockets TCP y hilos (`pthread`).

## Contexto académico

Proyecto del curso **Sistemas Operativos** de la **Universidad del Valle de Guatemala**.

## Integrantes

- Diego López - 23242
- Diego Rosales - 23258

## Descripción

Este proyecto implementa un sistema de chat con arquitectura cliente-servidor:

- El servidor acepta múltiples clientes de manera concurrente.
- Cada cliente puede enviar mensajes globales, mensajes privados y consultar información de usuarios.
- Se maneja estado de usuario (`ACTIVE`, `BUSY`, `INACTIVE`) y detección de inactividad.

## Estructura del proyecto

- `server.c`: lógica principal del servidor, manejo de conexiones y comandos.
- `client.c`: cliente interactivo por consola.
- `protocolo.h`: definición de comandos, estados y estructura de paquete (`ChatPacket`).

## Requisitos

- Compilador C compatible con POSIX (por ejemplo, `gcc`).
- Soporte de hilos (`pthread`).
- Entorno tipo Linux/Unix (Linux nativo, WSL o equivalente).

## Compilación

Desde la raíz del proyecto:

```bash
gcc -Wall server.c -o server -pthread
gcc -Wall client.c -o client -pthread
```

## Ejecución

1. Iniciar el servidor:

```bash
./server <puerto>
```

Ejemplo:

```bash
./server 5000
```

2. Iniciar uno o más clientes (en terminales distintas):

```bash
./client <username> <IP_servidor> <puerto>
```

Ejemplo local:

```bash
./client alice 127.0.0.1 5000
./client bob 127.0.0.1 5000
```

## Comandos disponibles en el cliente

- `/broadcast <mensaje>`: envía mensaje a todos los usuarios.
- `/msg <usuario> <mensaje>`: envía mensaje privado.
- `/status <ACTIVE|BUSY|INACTIVE>`: cambia tu estado.
- `/list`: lista usuarios conectados y su estado.
- `/info <usuario>`: muestra IP y estado de un usuario.
- `/help`: muestra la ayuda en pantalla.
- `/exit`: cierra sesión.

## Comportamientos importantes

- Registro inicial obligatorio con nombre de usuario.
- El servidor rechaza:
  - usuarios duplicados,
  - IPs duplicadas.
- Si un cliente está inactivo más de `60` segundos (`INACTIVITY_TIMEOUT`), el servidor cambia su estado a `INACTIVE`.
- Al desconectarse un usuario, el servidor notifica al resto con `CMD_DISCONNECTED`.

## Protocolo

La comunicación usa paquetes de tamaño fijo de 1024 bytes definidos en `ChatPacket`:

- `command`: tipo de comando.
- `payload_len`: longitud del contenido útil.
- `sender`: usuario emisor.
- `target`: usuario destino o `ALL`.
- `payload`: contenido del mensaje.

## Notas

- El cliente imprime mensajes asíncronos mediante un hilo receptor.
- El servidor protege la lista de clientes con `mutex` para acceso concurrente seguro.
- Para pruebas locales en Windows, se recomienda ejecutarlo en WSL.
