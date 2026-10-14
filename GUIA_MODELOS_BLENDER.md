# Guía: hacer un modelo nuevo de un coche con Blender y verlo en el juego

El juego puede dibujar **tu propio modelo 3D** de un coche en lugar del original. Esta guía explica cómo: sacar el
modelo del juego, cambiarlo en Blender y meterlo de vuelta. Los modelos salen de tu ROM: son solo para tu uso
personal y no deben compartirse.

---

## 1. Sacar el coche del juego

En una terminal, dentro de `C:\Users\jj\nfs-nitro-port`:

```
.venv\Scripts\python.exe tools\extract_models.py Graphics/Cars/Focus
```

(cambia `Focus` por el coche que quieras; la lista de nombres está en `notes/model_format.md` o en
`notes/data_inventory.md`). Se crea `C:\Users\jj\nfs-nitro-models\Focus\` con:

- `lod0.obj` … `lod3.obj`: la carrocería en 4 niveles de detalle. **En carrera tu coche usa `lod1`**: es el que
  hay que sustituir. `lod0` es el más detallado (el del garaje) y sirve de buena base.
- `wheel_lod1.obj`: la rueda que se ve en carrera.
- las texturas en PNG y unos `.json` con datos para volver a meterlo en el juego (no los borres).

## 2. Abrirlo en Blender

**File → Import → Wavefront (.obj)** y elige, por ejemplo, `lod1.obj`. Deja los ajustes por defecto (adelante -Z,
arriba Y).

Verás varios objetos con nombres como `polygon4__a545b3c9f799f6b1`. **Ese nombre es la clave**: la parte de
después de `__` es la huella con la que el juego reconoce esa pieza. Cada objeto es una pieza:

| Pieza del Focus `lod1` | Qué es |
|---|---|
| `polygon4__a545…` | la carrocería (lleva la pintura y el reflejo) |
| `polygon3__0bdb…` | detalles: faros, pilotos, logo, rejilla |
| `polygon1__f38d…` | cristales |
| `polygon0__11d4…`, `polygon2__7955…` | luces de freno |

## 3. Cambiarlo

Puedes hacer lo que quieras con la forma: más polígonos, subdividir, redondear, añadir detalles, o incluso
modelar la pieza desde cero. Reglas:

1. **No cambies el nombre de los objetos** (si Blender les añade `.001` no pasa nada).
2. **No muevas ni escales el coche entero**: tiene que quedar en el mismo sitio y del mismo tamaño que el original
   (el juego lo coloca con sus propias matrices).
3. **La pintura y la textura las pone el juego.** El modelo nuevo usa la misma textura que la pieza original: si
   añades geometría a la pieza de detalles, sus coordenadas UV tienen que apuntar a la misma imagen (por ejemplo,
   a `focDT1__focDT1_pl.png`). La carrocería no usa UV (su reflejo se calcula con las normales).
4. **Normales**: la luz y el reflejo de la carrocería dependen de ellas. Usa sombreado suave con aristas vivas
   (**Object → Shade Auto Smooth / Smooth by Angle**).
5. **No te pases de polígonos**: el 3D se dibuja por software. Unos miles de triángulos por coche van bien (el
   Focus de prueba tiene unos 4.000); decenas de miles no.
6. Si quieres usar como base el modelo de garaje (`lod0`), cambia el nombre de sus objetos al de la pieza de
   `lod1` que sustituyen (en el Focus: carrocería `polygon2` → `polygon4__a545b3c9f799f6b1`, detalles `polygon1`
   → `polygon3__0bdb89b1b607e137`, cristales `polygon0` → `polygon1__f38de5eb038ce1eb`).

## 4. Exportarlo

**File → Export → Wavefront (.obj)**, con los ajustes por defecto (adelante -Z, arriba Y) y marcando:
**Apply Modifiers**, **Normals**, **UV Coordinates**, **Triangulated Mesh**. Guárdalo donde quieras, por ejemplo
`C:\Users\jj\nfs-nitro-models\Focus\nuevo\mi_focus.obj`.

## 5. Meterlo en el juego

```
.venv\Scripts\python.exe tools\models\make_replacement.py C:\Users\jj\nfs-nitro-models\Focus\nuevo\mi_focus.obj --info C:\Users\jj\nfs-nitro-models\Focus
```

Dice cuántos triángulos tiene cada pieza nueva y las deja en `C:\Users\jj\nfs-nitro-models\reemplazos\`. Abre el
juego (con la casilla **"Modelos 3D nuevos"** del menú marcada) y corre con ese coche.

Para quitar un modelo nuevo: borra su `.nmesh` de `reemplazos\` (o desmarca la casilla para quitarlos todos).

## Prueba ya instalada

Hay un **Focus nuevo de prueba** instalado: el modelo de garaje con la carrocería subdividida en Blender (unos 4.000
triángulos en lugar de 309). Se ve más redondo y con más detalle (antena, paragolpes), pero **la parte de atrás
tiene un defecto**: el reflejo de la pintura forma vetas. Es el efecto de reflejo del propio juego, que con formas
más detalladas se ve así (detalles en `notes/model_format.md`). Compáralo con la casilla del menú.

## Si algo no sale

- **El coche se ve igual que antes:** ¿es la pieza de `lod1`? ¿Se llama igual que la original? ¿Está marcada la
  casilla? Para ver qué piezas manda el juego en cada momento: arrancar con `set NFS_DL_LOG=1` y mirar las
  líneas `[dl]` del registro `ultima_ejecucion.log`.
- **Sale descolocado o enorme:** se movió o escaló el coche, o se exportó con otros ejes.
- **Sale negro o con la textura mal:** UV que no apuntan a la textura de esa pieza, o normales invertidas.
