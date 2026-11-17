# Cómo publicar DAVIS Spectral Sidechain en Open MPC

Open MPC (https://open-mpc.github.io) es un catálogo: no aloja tu plugin, lo lee de **tu propio repositorio de GitHub**.
Tú publicas el código y un Release con el zip; luego abres un Pull Request con un único archivo JSON. A partir de ahí,
cada versión nueva que publiques aparece sola en el catálogo (lo revisa cada noche).

Todo lo que hace falta ya está en este pack. Solo tienes que hacerlo en este orden.

---

## 0. Antes de empezar: pruébalo en tu MPC
Todo lo de este paso está en la carpeta **`1-para-tu-MPC`** del pack.

1. Copia la carpeta **`DAVIS - VST - Spectral Sidechain`** (del zip `DAVIS-Spectral-Sidechain-<versión>-VST2-Plugins.zip`)
   dentro de la carpeta **`VST2 Plugins`** de tu MPC y reinicia.
2. Sigue la tabla de `TESTING.md` (11 pasos, unos 15 minutos).
3. Haz una **foto o captura de la pantalla de la MPC** con el plugin funcionando.
4. Apunta el modelo y la versión de MPC OS (por ejemplo: *MPC Live II, MPC OS 3.10.1*). En `TESTING.md`, última línea, rellena
   "Tested on".

Los mantenedores aceptan antes un plugin probado en un equipo real y con foto real.

---

## 1. Crea el repositorio en GitHub
1. Entra en https://github.com con tu cuenta (si no tienes, créala; es gratis).
2. Arriba a la derecha **+ → New repository**.
   - **Repository name:** `davis-spectral-sidechain`
   - **Public** (obligatorio para el catálogo).
   - No marques "Add a README" ni licencia (ya vienen en el pack).
   - **Create repository**.
3. En la página del repo vacío, pulsa **"uploading an existing file"**.
4. Descomprime `DAVIS-Spectral-Sidechain-source-<versión>.zip` (carpeta **`2-para-GitHub`**) en tu ordenador y **arrastra todo su contenido** (las carpetas
   `src`, `vst`, `tools`, `tests`, `wrapper`, `docs`, `.github` y los archivos `README.md`, `LICENSE`, `TESTING.md`…).
   - La carpeta **`.github`** está oculta en Mac/Windows. En Mac: en Finder pulsa `Cmd + Shift + .` para verla. En Windows:
     Explorador → Vista → "Elementos ocultos". **Tiene que subir**, es la que construye el plugin.
5. Abajo, **Commit changes**.
6. Sustituye `docs/preview.png` por tu foto real de la MPC cuando la tengas (mismo nombre, o añade `docs/device.png`).

## 2. Construye y publica el Release (lo hace GitHub por ti)
1. En tu repo: pestaña **Actions**. Si pregunta, pulsa **"I understand my workflows, go ahead and enable them"**.
2. A la izquierda, **Release** → botón **Run workflow**:
   - **Version:** `1.6.7`
   - **dry_run:** **desmarcado**
   - **Run workflow**.
3. Espera a que salga el check verde (unos minutos). Construye el plugin para la MPC con audio en 32 bits float, pasa los
   tests y crea un **Release en borrador** con el zip `Spectral-Sidechain-1.6.7-mpc-armv7.zip`.
4. Pestaña **Code → Releases** (columna derecha) → abre el borrador → escribe unas líneas (qué hace, en qué MPC lo probaste)
   → **Publish release**.

> ¿Por qué no subir a mano el zip que te he dado? Porque el zip del catálogo lleva dentro el nombre de tu repositorio
> (`usuario/davis-spectral-sidechain`), y GitHub Actions lo pone solo. Si prefieres hacerlo a mano, dime tu usuario de GitHub
> y te genero ese zip exacto.

Si el workflow falla en rojo: abre el paso en rojo, copia el error y pásamelo.

## 3. Pide que lo listen (Pull Request a Open MPC)
1. Ve a https://github.com/open-mpc/mpc-vst-plugins y pulsa **Fork** (arriba a la derecha) → **Create fork**.
2. En tu fork, entra en la carpeta **`catalog/plugins/`**.
3. **Add file → Create new file**. Nombre: **`spectral-sidechain.json`**.
4. Pega el contenido de `3-para-Open-MPC/spectral-sidechain.json` de este pack, cambiando **`TU_USUARIO_GITHUB`** por tu usuario
   (aparece 2 veces: en `repo` y en `screenshot`).
5. **Commit changes** → elige **"Create a new branch… and start a pull request"** → **Propose changes**.
6. En la página del Pull Request, título: `Add DAVIS Spectral Sidechain`. En la descripción pega el texto de
   `3-para-Open-MPC/PULL_REQUEST.md` (rellena modelo, versión de MPC OS y la foto). **Create pull request**.
7. GitHub pasa unas comprobaciones automáticas (que el JSON es válido y que tu último Release cumple las reglas). Si todo está
   en verde, un mantenedor lo revisa y lo acepta. Si te piden algo, responde en el mismo Pull Request.

## 4. Versiones futuras
Cuando cambies algo: actualiza el código en tu repo y vuelve a **Actions → Release → Run workflow** con un número mayor
(`1.6.8`, `1.7.0`…) y publica el borrador. **No hace falta otro Pull Request**: el catálogo lo detecta solo.

---

## Si ya subiste la 1.4.2
No crees otro repositorio. En tu repo `davis-spectral-sidechain`: **Add file → Upload files**, arrastra el contenido del zip de
código (sobrescribe los archivos; incluye `.github`), **Commit changes**, y haz el paso 2 con la versión `1.6.7`. Si el Pull
Request a Open MPC ya está aceptado, no hace falta nada más. Si aún está abierto, actualiza en él el resumen del JSON con el de
`3-para-Open-MPC/spectral-sidechain.json` (ya no habla de SC peak ni de TO FREQ).

## Reglas que conviene saber
- **Licencia:** MIT (archivo `LICENSE`). Si prefieres otra de código abierto, cámbiala en `LICENSE`, en
  `.github/workflows/release.yml` (`license:`) y en el JSON.
- **Marcas:** el nombre, el skin y las capturas no pueden usar marcas o logos de otras empresas. El tuyo (DAVIS, skin propio) está bien.
  La palabra "MPC" en la descripción es descriptiva, no como nombre del plugin.
- **Parámetros:** no cambies el orden de `vst/params.json` en versiones futuras (los proyectos guardados se romperían);
  lo nuevo, siempre al final. El build lo comprueba.
- **Compatibilidad:** MPC OS 3.x, equipos Gen1 (MPC Live, Live II, One, X, Force). No funciona en MPC OS 2.x.
