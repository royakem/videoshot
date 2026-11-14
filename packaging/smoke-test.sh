#!/bin/bash
# Runs inside a container that has the videoshot package, Xvfb and dbus
# installed. Records each format in a virtual display and prints one
# "SMOKE" line per check.
export DISPLAY=:99 GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 XDG_RUNTIME_DIR=/tmp/rt HOME=/tmp/home
mkdir -p -m 700 /tmp/rt /tmp/home
Xvfb :99 -screen 0 800x600x24 >/dev/null 2>&1 &
sleep 2
echo "SMOKE version: $(videoshot --version 2>/dev/null)"
cat > /tmp/inner.sh <<'EOS'
probe() { ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height:format=duration -of csv=p=0 "$1" 2>&1 | tr '\n' ' '; }
for f in mp4 webm mkv gif; do
  timeout 60 videoshot --video --length 2 --file /tmp/o.$f 2>/tmp/err.$f; rc=$?
  echo "SMOKE $f exit=$rc $(probe /tmp/o.$f)"
  [ $rc -ne 0 ] && grep -v "theme\|^$" /tmp/err.$f | head -3
done
( timeout 60 videoshot --video --length 30 --file /tmp/stop.gif 2>/dev/null; echo "SMOKE stop-gif exit=$?" ) &
sleep 4; videoshot --stop 2>/dev/null; wait
echo "SMOKE stop-gif $(probe /tmp/stop.gif)"
timeout 20 videoshot --file /tmp/shot.png 2>/dev/null; echo "SMOKE screenshot exit=$? $(probe /tmp/shot.png)"
EOS
dbus-run-session -- bash /tmp/inner.sh
