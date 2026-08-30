// Barracks+ worldserver pipeline.
//
// Mirrors the Legionnaire+ pipeline: same repo, same branch (both realms build
// from LEGIONNAIRE_PLUS - per-realm behaviour lives in Centurion.* config keys,
// never in branch edits), same RelWithDebInfo flags and the same PLAYERBOT
// cache verification.
//
// Two deliberate differences:
//   * INSTALL_DIR / SERVICE point at tc-barracksplus / barracksplusworld.
//   * ENABLE_PLAYERBOTS defaults to TRUE. Barracks+ is a playerbot realm; a
//     build with it off silently compiles the bots out and the realm comes up
//     empty, which has cost a rebuild cycle before.
properties([
  parameters([
    booleanParam(name: 'USE_PCH',           defaultValue: false, description: 'Use precompiled headers? (recommended OFF in CI)'),
    booleanParam(name: 'CLEAN_BUILD',       defaultValue: false, description: 'Delete build dir before configure'),
    booleanParam(name: 'ENABLE_PLAYERBOTS', defaultValue: true,  description: 'Build TrinityCore with PLAYERBOT enabled (Barracks+ needs this ON)')
  ])
])

node {
  def SRC_DIR     = env.WORKSPACE
  def BUILD_DIR   = "${env.WORKSPACE}/build"
  def INSTALL_DIR = "/home/brokilodeluxe/wow/servers/tc-barracksplus"
  def SERVICE     = "barracksplusworld"

  try {
    stage('Preparation') {
      checkout([
        $class: 'GitSCM',
        branches: [[name: '*/LEGIONNAIRE_PLUS']],
        userRemoteConfigs: [[
          url: 'https://github.com/thomasjteachey/TrinityCore112'
        ]]
      ])
    }

    if (params.CLEAN_BUILD) {
      stage('Clean build dir (requested)') {
        withEnv(["BUILD_DIR=${BUILD_DIR}"]) {
          sh '''#!/usr/bin/env bash
set -Eeuo pipefail

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
'''
        }
      }
    } else {
      stage('Ensure build dir') {
        withEnv(["BUILD_DIR=${BUILD_DIR}"]) {
          sh '''#!/usr/bin/env bash
set -Eeuo pipefail

mkdir -p "$BUILD_DIR"
'''
        }
      }
    }

    withEnv([
      "SRC_DIR=${SRC_DIR}",
      "BUILD_DIR=${BUILD_DIR}",
      "INSTALL_DIR=${INSTALL_DIR}",
      "SERVICE=${SERVICE}",

      params.USE_PCH
        ? 'PCH_CMAKE=-DCMAKE_DISABLE_PRECOMPILE_HEADERS=OFF -DPCH=1'
        : 'PCH_CMAKE=-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DPCH=0',

      params.USE_PCH
        ? 'USE_PCH=1'
        : 'USE_PCH=0',

      'BUILD_TYPE=RelWithDebInfo',
      'DEBUG_DEFINE=-DDEBUG=0',

      params.ENABLE_PLAYERBOTS
        ? 'ENABLE_PLAYERBOTS=1'
        : 'ENABLE_PLAYERBOTS=0',

      params.ENABLE_PLAYERBOTS
        ? 'PLAYERBOT_FLAG=-DPLAYERBOT=ON'
        : 'PLAYERBOT_FLAG=-DPLAYERBOT=OFF'
    ]) {

      stage('Configure') {
        sh '''#!/usr/bin/env bash
set -Eeuo pipefail

PREFIX="$INSTALL_DIR"
CONFIGURE_LOG="$BUILD_DIR/configure.log"
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"

GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  GEN_ARGS=(-G Ninja)
fi

LAUNCHER_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  if [ "$USE_PCH" = "1" ]; then
    export CCACHE_DISABLE=1
  else
    LAUNCHER_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache")
    LAUNCHER_ARGS+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
  fi
fi

BUILD_TYPE_ARGS=(
  "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g3 -gdwarf-4 -fno-omit-frame-pointer"
  "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g3 -gdwarf-4 -fno-omit-frame-pointer"
  "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=-rdynamic"
)

do_configure() {
  cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${GEN_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    $PCH_CMAKE \
    $DEBUG_DEFINE \
    $PLAYERBOT_FLAG \
    "${BUILD_TYPE_ARGS[@]}" \
    "${LAUNCHER_ARGS[@]}"
}

: > "$CONFIGURE_LOG"

echo "[INFO] Build type: RelWithDebInfo" | tee -a "$CONFIGURE_LOG"
echo "[INFO] Debug builds are disabled by this pipeline." | tee -a "$CONFIGURE_LOG"
echo "[INFO] USE_PCH: $USE_PCH" | tee -a "$CONFIGURE_LOG"
echo "[INFO] ENABLE_PLAYERBOTS: ${ENABLE_PLAYERBOTS:-0}" | tee -a "$CONFIGURE_LOG"

do_configure 2>&1 | tee -a "$CONFIGURE_LOG"

if [ "$USE_PCH" = "1" ]; then
  if ! find "$BUILD_DIR" -type f -name 'cmake_pch.hxx.cxx' | grep -q .; then
    echo "[PCH] cmake_pch.hxx.cxx missing; re-running CMake configure..." |
      tee -a "$CONFIGURE_LOG"

    do_configure 2>&1 | tee -a "$CONFIGURE_LOG"
  fi
fi

if [ ! -f "$CACHE_FILE" ]; then
  echo "[ERROR] Missing CMake cache: $CACHE_FILE"
  exit 1
fi

EXPECTED_PLAYERBOT="OFF"
if [ "${ENABLE_PLAYERBOTS:-0}" = "1" ]; then
  EXPECTED_PLAYERBOT="ON"
fi

if ! grep -Eq "^PLAYERBOT(:[^=]+)?=$EXPECTED_PLAYERBOT$" "$CACHE_FILE"; then
  echo "[ERROR] Expected PLAYERBOT=$EXPECTED_PLAYERBOT in CMakeCache.txt"
  grep -E '^PLAYERBOT(:[^=]+)?=' "$CACHE_FILE" || true
  exit 1
fi

if ! grep -Eq '^CMAKE_BUILD_TYPE(:[^=]+)?=RelWithDebInfo$' "$CACHE_FILE"; then
  echo "[ERROR] Expected CMAKE_BUILD_TYPE=RelWithDebInfo in CMakeCache.txt"
  grep -E '^CMAKE_BUILD_TYPE(:[^=]+)?=' "$CACHE_FILE" || true
  exit 1
fi

if grep -Eq '^CMAKE_BUILD_TYPE(:[^=]+)?=Debug$' "$CACHE_FILE"; then
  echo "[ERROR] Debug build detected. This pipeline only permits RelWithDebInfo."
  exit 1
fi

echo "[OK] PLAYERBOT is $EXPECTED_PLAYERBOT in CMake cache."
echo "[OK] CMAKE_BUILD_TYPE is RelWithDebInfo."
'''
      }

      stage('Build') {
        sh '''#!/usr/bin/env bash
set -Eeuo pipefail

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
'''
      }

      stage('Install & Restart') {
        sh '''#!/usr/bin/env bash
set -Eeuo pipefail

sudo systemctl stop "$SERVICE" || true
sudo cmake --install "$BUILD_DIR"
sudo systemctl daemon-reload || true
sudo systemctl start "$SERVICE"

sleep 2

sudo systemctl --no-pager --full status "$SERVICE" || true
'''
      }

      stage('Fix install perms') {
        sh '''#!/usr/bin/env bash
set -Eeuo pipefail

if [ -d "$INSTALL_DIR" ]; then
  sudo chown -R "brokilodeluxe:brokilodeluxe" "$INSTALL_DIR" || true
fi
'''
      }
    }

  } finally {
    stage('Diagnostics') {
      withEnv([
        "SERVICE=${SERVICE}",
        "INSTALL_DIR=${INSTALL_DIR}"
      ]) {
        sh '''#!/usr/bin/env bash
set +e

echo "===== systemctl status: $SERVICE ====="
sudo systemctl --no-pager --full status "$SERVICE" || true

echo
echo "===== recent journalctl for $SERVICE ====="
sudo journalctl -u "$SERVICE" -n 200 --no-pager || true

echo
echo "===== recent coredump entries ====="
if command -v coredumpctl >/dev/null 2>&1; then
  sudo coredumpctl list --no-pager | tail -n 20 || true
else
  echo "coredumpctl not found"
fi

echo
echo "===== worldserver binary info ====="
if [ -f "$INSTALL_DIR/bin/worldserver" ]; then
  ls -l "$INSTALL_DIR/bin/worldserver" || true
  file "$INSTALL_DIR/bin/worldserver" || true

  readelf -S "$INSTALL_DIR/bin/worldserver" |
    egrep 'debug_info|debug_line|symtab|dynsym' || true
else
  echo "worldserver not found at $INSTALL_DIR/bin/worldserver"
fi

exit 0
'''
      }
    }
  }
}
