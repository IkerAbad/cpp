#!/usr/bin/env bash
# Configura con CMake (en la configuración, vcpkg instala las dependencias) y, si lo que
# falla es una descarga de vcpkg, lo reintenta hasta 3 veces con espera creciente.
# vcpkg baja de github.com algunos parches (commit/<sha>.patch) que a veces responden
# 503 varias veces seguidas; lo ya compilado queda en la caché binaria
# (VCPKG_DEFAULT_BINARY_CACHE), así que cada intento sigue desde ahí. Otros errores de
# configuración fallan a la primera.
#
# Uso: configurar.sh <preset> [más argumentos de cmake...]
set -euo pipefail
preset="$1"
shift
espera_s="${CONFIGURAR_ESPERA_S:-60}"
log="$(mktemp)"
for intento in 1 2 3; do
    if cmake --preset "$preset" "$@" 2>&1 | tee "$log"; then
        exit 0
    fi
    if ! grep -q "Download failed" "$log"; then
        exit 1
    fi
    echo "::warning::vcpkg no pudo descargar algo (intento $intento de 3, preset $preset)"
    # Sin la carpeta de la configuración fallida: CMake no arrastra lo que dejó a medias.
    rm -rf "build/$preset"
    if [ "$intento" -lt 3 ]; then
        sleep $((espera_s * intento))
    fi
done
exit 1
