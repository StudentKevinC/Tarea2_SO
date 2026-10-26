#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#define PERROS 12
#define FICHAS 13
#define CASILLAS 28


typedef struct {
    int min_ms, max_ms;
    int puma;
    int perros[PERROS];
    unsigned int seed;
} Config;


typedef struct Partida Partida;

struct Partida {
    int numero;
    int jugadores[2];

    int tablero[CASILLAS];
    int posicion[FICHAS];

    int capturas, movimientos;
    int ganador, terminada, lista, abandono;

    pthread_mutex_t mutex;
    pthread_cond_t cambio;

    pthread_t piezas[FICHAS];
    pthread_t juez;

    Config config;
};


typedef struct {
    Partida *partida;
    int ficha;
    unsigned int semilla;
} DatoFicha;


typedef struct {
    int id;
    Partida *partida;
} Cliente;



static volatile sig_atomic_t detener = 0;

static pthread_mutex_t emparejar_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t imprimir_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t emparejar_cond = PTHREAD_COND_INITIALIZER;

static sem_t mesas_libres;

static int esperando = -1;
static int numero_partidas = 0;

static Cliente *clientes;
static Config config;



// Para detener el torneo con Ctrl+C
static void senal(int sig) {
    (void)sig;
    detener = 1;
}


static void dormir_ms(int ms) {
    struct timespec t;

    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&t, &t) == -1 && errno == EINTR && !detener) {
    }
}


static int azar(unsigned int *semilla, int min, int max) {
    if (max <= min) return min;

    return min + (int)(rand_r(semilla) % (unsigned int)(max - min + 1));
}



// Valores normales del juego
static void config_inicial(Config *c) {
    c->min_ms = 50;
    c->max_ms = 300;
    c->seed = 1234;
    c->puma = 12;

    for (int i = 0; i < PERROS; i++)
        c->perros[i] = i;
}


static int leer_config(const char *ruta, Config *c) {
    FILE *f = fopen(ruta, "r");
    if (!f) return -1;

    char linea[512], clave[80], valor[400];

    while (fgets(linea, sizeof(linea), f)) {
        if (sscanf(linea, " %79[^=]=%399s", clave, valor) != 2)
            continue;

        if (strcmp(clave, "min_ms") == 0)
            c->min_ms = atoi(valor);

        else if (strcmp(clave, "max_ms") == 0)
            c->max_ms = atoi(valor);

        else if (strcmp(clave, "seed") == 0)
            c->seed = (unsigned int)strtoul(valor, NULL, 10);

        else if (strcmp(clave, "puma") == 0)
            c->puma = atoi(valor);

        else if (strcmp(clave, "perros") == 0) {
            char *parte = strtok(valor, ",");

            for (int i = 0; i < PERROS; i++) {
                if (!parte) {
                    fclose(f);
                    return -1;
                }

                c->perros[i] = atoi(parte);
                parte = strtok(NULL, ",");
            }

            if (parte) {
                fclose(f);
                return -1;
            }
        }
    }

    fclose(f);

    if (c->min_ms < 1 || c->max_ms < c->min_ms ||
        c->puma < 0 || c->puma >= CASILLAS)
        return -1;

    int ocupado[CASILLAS] = {0};
    ocupado[c->puma] = 1;

    for (int i = 0; i < PERROS; i++) {
        int pos = c->perros[i];

        if (pos < 0 || pos >= CASILLAS || ocupado[pos])
            return -1;

        ocupado[pos] = 1;
    }

    return 0;
}



// Posiciones 0 a 24: tablero de 5x5
// 25 y 26: parte del trono
// 27: punta superior

static void coordenadas(int casilla, int *fila, int *col) {
    if (casilla < 25) {
        *fila = casilla / 5 + 2;
        *col = casilla % 5;
    }
    else if (casilla == 25) {
        *fila = 1;
        *col = 1;
    }
    else if (casilla == 26) {
        *fila = 1;
        *col = 3;
    }
    else {
        *fila = 0;
        *col = 2;
    }
}


// Revisa si hay una linea entre dos casillas
static int vecino(int a, int b) {
    if (a < 0 || b < 0 || a >= CASILLAS ||
        b >= CASILLAS || a == b)
        return 0;

    if (a < 25 && b < 25) {
        int f1 = a / 5, c1 = a % 5;
        int f2 = b / 5, c2 = b % 5;

        return abs(f1 - f2) + abs(c1 - c2) == 1;
    }

    // Conexiones del trono segun el dibujo
    int lineas[][2] = {
        {25, 0}, {25, 1}, {25, 2},
        {26, 2}, {26, 3}, {26, 4},
        {25, 26}, {25, 27}, {26, 27}
    };

    int total = (int)(sizeof(lineas) / sizeof(lineas[0]));

    for (int i = 0; i < total; i++) {
        if ((a == lineas[i][0] && b == lineas[i][1]) ||
            (b == lineas[i][0] && a == lineas[i][1]))
            return 1;
    }

    return 0;
}


// El Puma salta sobre un Perro si hay espacio
static int captura_valida(Partida *p, int origen, int medio, int destino) {
    if (origen == medio || medio == destino || origen == destino)
        return 0;

    if (!vecino(origen, medio) || !vecino(medio, destino))
        return 0;

    if (p->tablero[medio] <= 0 || p->tablero[destino] != 0)
        return 0;

    int f1, c1, f2, c2, f3, c3;

    coordenadas(origen, &f1, &c1);
    coordenadas(medio, &f2, &c2);
    coordenadas(destino, &f3, &c3);

    return f2 - f1 == f3 - f2 && c2 - c1 == c3 - c2;
}


static int puma_tiene_movimientos(Partida *p) {
    int origen = p->posicion[0];

    for (int i = 0; i < CASILLAS; i++) {
        if (p->tablero[i] == 0 && vecino(origen, i))
            return 1;
    }

    for (int medio = 0; medio < CASILLAS; medio++) {
        for (int destino = 0; destino < CASILLAS; destino++) {
            if (captura_valida(p, origen, medio, destino))
                return 1;
        }
    }

    return 0;
}



// Muestra como va la partida
static void mostrar(Partida *p) {
    pthread_mutex_lock(&imprimir_mutex);

    printf("\nMesa %d | Movimiento %d | Capturas %d\n",
           p->numero, p->movimientos, p->capturas);

    char s[CASILLAS];

    for (int i = 0; i < CASILLAS; i++) {
        if (p->tablero[i] == -1) s[i] = 'P';
        else if (p->tablero[i] > 0) s[i] = 'D';
        else s[i] = '.';
    }

    printf("    %c\n", s[27]);
    printf("  %c   %c\n", s[25], s[26]);

    for (int f = 0; f < 5; f++) {
        for (int c = 0; c < 5; c++)
            printf("%c ", s[f * 5 + c]);

        printf("\n");
    }

    fflush(stdout);
    pthread_mutex_unlock(&imprimir_mutex);
}



// Se llama mientras esta tomado el mutex de la partida
static void mover(Partida *p, int ficha, unsigned int *semilla) {
    int origen = p->posicion[ficha];
    if (origen < 0) return;

    int destinos[CASILLAS * CASILLAS];
    int saltos[CASILLAS * CASILLAS];
    int opciones = 0;

    for (int i = 0; i < CASILLAS; i++) {
        if (p->tablero[i] == 0 && vecino(origen, i)) {
            destinos[opciones] = i;
            saltos[opciones] = -1;
            opciones++;
        }
    }

    if (ficha == 0) {
        for (int medio = 0; medio < CASILLAS; medio++) {
            for (int destino = 0; destino < CASILLAS; destino++) {
                if (captura_valida(p, origen, medio, destino)) {
                    destinos[opciones] = destino;
                    saltos[opciones] = medio;
                    opciones++;
                }
            }
        }
    }

    if (opciones == 0) return;

    int elegido = azar(semilla, 0, opciones - 1);
    int destino = destinos[elegido];
    int salto = saltos[elegido];

    p->tablero[origen] = 0;
    p->tablero[destino] = ficha == 0 ? -1 : ficha;
    p->posicion[ficha] = destino;

    if (salto >= 0) {
        int perro = p->tablero[salto];

        p->posicion[perro] = -1;
        p->tablero[salto] = 0;
        p->capturas++;
    }

    p->movimientos++;

    if (p->capturas >= 6) {
        p->ganador = 1;
        p->terminada = 1;
    }
    else if (!puma_tiene_movimientos(p)) {
        p->ganador = 2;
        p->terminada = 1;
    }

    mostrar(p);
    pthread_cond_broadcast(&p->cambio);
}



static void *ejecutar_pieza(void *arg) {
    DatoFicha *d = arg;
    Partida *p = d->partida;
    unsigned int semilla = d->semilla;

    while (!detener) {
        dormir_ms(azar(&semilla, p->config.min_ms, p->config.max_ms));

        pthread_mutex_lock(&p->mutex);

        if (detener || p->terminada) {
            pthread_mutex_unlock(&p->mutex);
            break;
        }

        mover(p, d->ficha, &semilla);
        pthread_mutex_unlock(&p->mutex);
    }

    return NULL;
}


static void iniciar_tablero(Partida *p) {
    memset(p->tablero, 0, sizeof(p->tablero));

    p->posicion[0] = p->config.puma;
    p->tablero[p->config.puma] = -1;

    for (int i = 0; i < PERROS; i++) {
        int pos = p->config.perros[i];
        p->posicion[i + 1] = pos;
        p->tablero[pos] = i + 1;
    }
}



// Esto sirve para probar un abandono
// Ejemplo: FALLAR_JUGADOR=1 ./torneo 2 1 config.txt
static int abandono_configurado(Partida *p) {
    const char *texto = getenv("FALLAR_JUGADOR");
    if (!texto || !*texto) return 0;

    char *fin;
    long id = strtol(texto, &fin, 10);

    if (*fin || id <= 0 || id > INT_MAX) return 0;

    if (id == p->jugadores[0]) return 2;
    if (id == p->jugadores[1]) return 1;

    return 0;
}



// El juez espera una mesa y controla la partida
static void *ejecutar_juez(void *arg) {
    Partida *p = arg;
    DatoFicha datos[FICHAS];

    int creadas = 0;
    int tiene_mesa = 0;

    while (!detener) {
        struct timespec limite;
        clock_gettime(CLOCK_REALTIME, &limite);
        limite.tv_sec++;

        if (sem_timedwait(&mesas_libres, &limite) == 0) {
            tiene_mesa = 1;
            break;
        }

        if (errno != ETIMEDOUT && errno != EINTR)
            break;
    }

    if (!tiene_mesa || detener) {
        if (tiene_mesa) sem_post(&mesas_libres);
        goto terminar;
    }

    pthread_mutex_lock(&p->mutex);

    iniciar_tablero(p);
    mostrar(p);

    if (!puma_tiene_movimientos(p)) {
        p->ganador = 2;
        p->terminada = 1;
    }

    pthread_mutex_unlock(&p->mutex);

    for (int i = 0; i < FICHAS && !detener; i++) {
        pthread_mutex_lock(&p->mutex);
        int terminada = p->terminada;
        pthread_mutex_unlock(&p->mutex);

        if (terminada) break;

        datos[i].partida = p;
        datos[i].ficha = i;
        datos[i].semilla = p->config.seed +
                           (unsigned int)(p->numero * 100 + i);

        if (pthread_create(&p->piezas[i], NULL,
                           ejecutar_pieza, &datos[i]) != 0) {
            pthread_mutex_lock(&p->mutex);
            p->terminada = 1;
            pthread_mutex_unlock(&p->mutex);
            break;
        }

        creadas++;
    }

    pthread_mutex_lock(&p->mutex);

    while (!p->terminada && !detener) {
        struct timespec limite;
        clock_gettime(CLOCK_REALTIME, &limite);

        limite.tv_nsec += 100000000L;
        if (limite.tv_nsec >= 1000000000L) {
            limite.tv_sec++;
            limite.tv_nsec -= 1000000000L;
        }

        pthread_cond_timedwait(&p->cambio, &p->mutex, &limite);

        int ganador = abandono_configurado(p);

        if (ganador) {
            p->ganador = ganador;
            p->abandono = 1;
            p->terminada = 1;
        }
    }

    p->terminada = 1;
    pthread_mutex_unlock(&p->mutex);

    for (int i = 0; i < creadas; i++)
        pthread_join(p->piezas[i], NULL);

    pthread_mutex_lock(&imprimir_mutex);

    if (p->abandono)
        printf("\nMesa %d: gano %s por abandono\n",
               p->numero, p->ganador == 1 ? "Puma" : "Perros");

    else if (p->ganador)
        printf("\nMesa %d: ganaron %s\n",
               p->numero, p->ganador == 1 ? "Puma" : "Perros");

    else
        printf("\nMesa %d: partida interrumpida\n", p->numero);

    pthread_mutex_unlock(&imprimir_mutex);

    sem_post(&mesas_libres);

terminar:
    pthread_mutex_lock(&p->mutex);
    p->lista = 1;
    pthread_cond_broadcast(&p->cambio);
    pthread_mutex_unlock(&p->mutex);

    return NULL;
}



// Los jugadores llegan de a dos
static void *ejecutar_cliente(void *arg) {
    Cliente *c = arg;

    pthread_mutex_lock(&emparejar_mutex);

    if (detener) {
        pthread_mutex_unlock(&emparejar_mutex);
        return NULL;
    }

    if (esperando == -1) {
        esperando = c->id;

        while (!detener && c->partida == NULL) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);

            t.tv_nsec += 100000000L;
            if (t.tv_nsec >= 1000000000L) {
                t.tv_sec++;
                t.tv_nsec -= 1000000000L;
            }

            pthread_cond_timedwait(&emparejar_cond,
                                   &emparejar_mutex, &t);
        }

        if (esperando == c->id) esperando = -1;

        Partida *p = c->partida;
        pthread_mutex_unlock(&emparejar_mutex);

        if (!p) return NULL;

        pthread_mutex_lock(&p->mutex);
        while (!p->lista)
            pthread_cond_wait(&p->cambio, &p->mutex);
        pthread_mutex_unlock(&p->mutex);

        return NULL;
    }

    int primero = esperando;
    esperando = -1;

    Partida *p = calloc(1, sizeof(*p));

    if (!p) {
        detener = 1;
        pthread_cond_broadcast(&emparejar_cond);
        pthread_mutex_unlock(&emparejar_mutex);
        return NULL;
    }

    p->numero = ++numero_partidas;
    p->jugadores[0] = primero;
    p->jugadores[1] = c->id;
    p->config = config;

    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->cambio, NULL);

    clientes[primero - 1].partida = p;
    c->partida = p;

    pthread_cond_broadcast(&emparejar_cond);
    pthread_mutex_unlock(&emparejar_mutex);

    pthread_mutex_lock(&imprimir_mutex);
    printf("\nPareja %d: jugador %d (Puma) y jugador %d (Perros)\n",
           p->numero, primero, c->id);
    pthread_mutex_unlock(&imprimir_mutex);

    if (pthread_create(&p->juez, NULL, ejecutar_juez, p) == 0) {
        pthread_join(p->juez, NULL);
    }
    else {
        pthread_mutex_lock(&p->mutex);
        p->lista = 1;
        pthread_cond_broadcast(&p->cambio);
        pthread_mutex_unlock(&p->mutex);
    }

    pthread_mutex_lock(&p->mutex);
    p->lista = 1;
    pthread_cond_broadcast(&p->cambio);
    pthread_mutex_unlock(&p->mutex);

    return NULL;
}



int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Uso: %s N M [config.txt]\n", argv[0]);
        return 1;
    }

    char *fin1, *fin2;
    long n = strtol(argv[1], &fin1, 10);
    long m = strtol(argv[2], &fin2, 10);

    if (!*argv[1] || *fin1 || !*argv[2] || *fin2 ||
        n < 2 || n % 2 || n > 2000 || m < 1 || m > 2000) {
        fprintf(stderr, "N debe ser par (2 a 2000) y M entre 1 y 2000\n");
        return 1;
    }

    config_inicial(&config);

    if (argc == 4 && leer_config(argv[3], &config)) {
        fprintf(stderr, "Configuracion incorrecta\n");
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = senal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    if (sem_init(&mesas_libres, 0, (unsigned int)m) != 0) {
        perror("sem_init");
        return 1;
    }

    clientes = calloc((size_t)n, sizeof(*clientes));
    pthread_t *hilos = calloc((size_t)n, sizeof(*hilos));

    if (!clientes || !hilos) {
        free(clientes);
        free(hilos);
        sem_destroy(&mesas_libres);
        return 1;
    }

    int creados = 0;

    for (int i = 0; i < n && !detener; i++) {
        clientes[i].id = i + 1;

        if (pthread_create(&hilos[i], NULL,
                           ejecutar_cliente, &clientes[i]) != 0) {
            detener = 1;
            break;
        }

        creados++;
    }

    for (int i = 0; i < creados; i++)
        pthread_join(hilos[i], NULL);

    for (int i = 0; i < creados; i++) {
        Partida *p = clientes[i].partida;

        if (p && p->jugadores[1] == clientes[i].id) {
            pthread_cond_destroy(&p->cambio);
            pthread_mutex_destroy(&p->mutex);
            free(p);
        }
    }

    free(clientes);
    free(hilos);

    sem_destroy(&mesas_libres);
    pthread_cond_destroy(&emparejar_cond);
    pthread_mutex_destroy(&emparejar_mutex);
    pthread_mutex_destroy(&imprimir_mutex);

    printf("\nTorneo finalizado\n");
    return 0;
}