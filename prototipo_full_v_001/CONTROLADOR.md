# Controlador hidráulico

Al encender, el equipo trabaja con la distribución de este documento. Si hay WiFi, descarga las reglas del portal y las vuelve a consultar cada 5 minutos (`NET_SYNC_MS`). Sin clave de red abre Bluetooth `AquaTech_Proto` y se queda con estos valores. El radio solo se enciende durante esa consulta.

## Salidas

El relevador enciende en HIGH.

| Salida | GPIO | Cómo arranca | Valor si el portal no manda otra cosa |
| --- | --- | --- | --- |
| Bomba de agua cruda | 32 | Interruptor entre GPIO 16 y GND | Activa |
| Válvula solenoide | 33 | El mismo interruptor GPIO 16 | Activa. La válvula y la bomba se detonan juntas |
| Flush | 25 | Tiempo | 30 segundos cada 12 horas |
| Bomba de cloro | 26 | Flujo | Enciende si el agua cruda a tienda es mayor a 0 |
| Libre | 27 | — | Apagada. Antes era la salida de presión |
| Bomba de agua purificada | 14 | Interruptor entre GPIO 22 y GND | Activa |

GPIO 16 y GPIO 22 no se comparten. Los dos interruptores cierran a GND.

## Flujos

Pulso a GND, 0.1 L por pulso.

| Flujo | Dónde se lee | Para qué sirve |
| --- | --- | --- |
| Agua cruda | GPIO 4 | Si no hay flujo, no arranca equipo |
| Agua cruda a tienda | GPIO 19 | Mayor a 0 enciende la bomba de cloro |
| Rechazo | Medidor de la plataforma | Solo cálculo |
| Producción | Medidor de la plataforma | Solo cálculo |
| Agua purificada a tienda | Medidor de la plataforma | Solo lectura. La bomba se enciende con el interruptor |

El sketch guarda dos medidores físicos. Rechazo, producción y purificada a tienda todavía no entran en la tarjeta.

## Presión

Se mide y se muestra. No enciende relevadores.

| Sensor | GPIO | Fondo de escala |
| --- | --- | --- |
| S1 | 34 | 0.812 MPa |
| S2 | 35 | 1.983 MPa |
| S3 | 13 | 0.915 MPa |
| S4 | 12 | 2.327 MPa |
| S5 | sin pin | — |

GPIO 12 y GPIO 13 también son salidas de la placa. Se leen solo con el WiFi apagado y vuelven a LOW.

## Lo que numera el portal

En el formulario el orden es otro, porque la válvula y la bomba de cruda son una sola regla:

| Regla del portal | Equipo | Corresponde en la placa |
| --- | --- | --- |
| 1 | Válvula y bomba de agua cruda | GPIO 32 y GPIO 33, interruptor 16 |
| 2 | Flush | GPIO 25 |
| 3 | Bomba de cloro | GPIO 26, medidor GPIO 19 |
| 4 | Bomba de agua purificada | GPIO 14, interruptor 22 |

El sketch todavía aplica el número de regla del portal sobre la salida del mismo número. No guardes estas reglas en el equipo que está corriendo hasta que esa copia respete la tabla de arriba.

## Pines que no se usan para esto

GPIO 2, 5, 18, 21 y 23 son la pantalla. GPIO 15 interviene en el arranque. GPIO 21 no sirve como medidor de pulsos.
