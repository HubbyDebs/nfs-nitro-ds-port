#!/bin/sh
# Arranca el juego recompilado; si se para por una funcion sin traducir, recompila y repite.
# Uso: sh tools/recomp/run_loop.sh [veces]
PROJECT=/c/Users/jj/nfs-nitro-port
BUILD=/c/Users/jj/nfs-nitro-build/game
N=${1:-12}
i=0
while [ $i -lt $N ]; do
  i=$((i+1))
  cd $BUILD && timeout 120 ./nfs_boot.exe 'C:\nfs' > run.log 2>&1
  if grep -q "no hay funcion traducida" run.log; then
    grep "llamada indirecta" run.log
    cd $PROJECT && ./.venv/Scripts/python.exe tools/recomp/build.py > $BUILD/build.log 2>&1 || { tail -20 $BUILD/build.log; exit 1; }
  else
    break
  fi
done
echo "--- vueltas: $i"
cat $BUILD/run.log | head -${2:-80}
