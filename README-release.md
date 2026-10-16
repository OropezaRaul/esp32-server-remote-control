# Directorio de release publica

`public-release/` es la copia del firmware que se publica: **solo codigo funcional,
sin nada que identifique una instalacion real**. No se edita a mano.

## Que contiene y que NO

| | |
|---|---|
| Codigo fuente completo del firmware (compilable por si mismo) | si |
| Tests host | si |
| Documentacion publica de uso (README.txt) | si |
| Comentarios tecnicos del desarrollo | **no** |
| Bitacora, planes y notas de campo (STATUS.md, docs/) | **no** |
| Directorio de red de la instalacion (RED_ENLACES.md) | **no** |
| IPs, MAC, hostnames, dominios y nombres de dispositivo reales | **no** |
| Nombres del negocio en simbolos, rutas y claves JSON | **no** |
| Credenciales | nunca (van en `sdkconfig.credentials`, gitignored) |

Los simbolos internos que llevaban el nombre del negocio se renombran al generico
del servidor (tambien los nombres de archivo, las claves JSON de `/status` y
`/diag`, y las rutas de ejemplo).

**Consecuencia**: un firmware compilado desde este directorio reporta `/status` y
`/diag` con las claves `server_*` en vez de las del desarrollo. Es el mismo firmware
con otros nombres de variables, no otra version.

## Como se regenera

```bash
tools/make-public-release.sh          # reconstruye public-release/ desde cero
```

El script exporta los archivos versionados, sanea (sin comentarios de bloque, sin
valores reales, sin nombres del negocio), reinstala los assets publicos mantenidos a
mano, regenera el certificado TLS de respaldo con un CN generico y **aborta** si
detecta cualquier patron prohibido.

Archivos que lo sostienen:

| Archivo | Rol |
|---|---|
| `tools/make-public-release.sh` | orquesta la generacion |
| `tools/public_sanitize.py` | comentarios, valores y renombres |
| `tools/public_test_cert.py` | certificado autofirmado genérico |
| `tools/public_verify.py` | falla si queda un patron privado |
| `tools/public-assets/` | README publico y helper del servidor (ingles, genericos) |

## Verificacion antes de publicar

```bash
tools/make-public-release.sh
cd public-release && ./idf.sh build      # compila sin credenciales
for f in tools/tests/run_*_test.sh; do bash $f; done
python3 tools/public_verify.py public-release
```
