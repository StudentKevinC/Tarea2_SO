# Tarea 2 - Torneo de Komikan Concurrente

## Descripción

Este programa simula un torneo de Komikan usando hilos en C.

Los jugadores se van emparejando de a dos para jugar una partida. Cada partida tiene un Puma y 12 Perros, que se mueven de manera independiente y sin turnos.

El programa también controla cuántas mesas pueden estar funcionando al mismo tiempo.

## Cómo compilar

Desde la terminal de Ubuntu, dentro de la carpeta del proyecto:

```bash
make
```

## Cómo ejecutar

```bash
./torneo N M [config.txt]
```

Donde:
- `N` es la cantidad de jugadores (debe ser par).
- `M` es el máximo de mesas que pueden jugar al mismo tiempo.
- `config.txt` es opcional y permite cambiar algunos valores.

Por ejemplo:

```bash
./torneo 6 2 config.txt
```

En este caso participan 6 jugadores, se forman 3 partidas y pueden jugar como máximo 2 mesas a la vez.

## Configuración

En `config.txt` se pueden cambiar los tiempos de movimiento, la semilla aleatoria y las posiciones iniciales de las fichas.

```ini
min_ms=50
max_ms=300
seed=1234
puma=12
perros=0,1,2,3,4,5,6,7,8,9,10,11
```

Las posiciones del 0 al 24 son las del tablero de 5x5 y las posiciones 25, 26 y 27 corresponden al trono.

## Cómo funciona

Para hacer funcionar el torneo usamos hilos (`pthread`). Cada jugador tiene su hilo, al igual que cada ficha y el árbitro de cada partida.

Usamos un mutex para que dos fichas no modifiquen el tablero al mismo tiempo. También usamos un semáforo para controlar las mesas disponibles y variables de condición para que los jugadores puedan esperar.

El Puma gana cuando captura 6 Perros