# Upload firmware lewat kabel LAN (lihat src/lan_update.cpp).
# Password dibaca dari include/secrets.h, jadi tidak perlu variabel lingkungan.
import re
Import("env")

def lan_password():
    text = open(env.subst("$PROJECT_DIR/include/secrets.h"), encoding="utf-8").read()
    m = re.search(r'LAN_UPDATE_PASSWORD\s+"([^"]+)"', text)
    if not m:
        raise SystemExit("LAN_UPDATE_PASSWORD tidak ada di include/secrets.h")
    return m.group(1)

env.Replace(UPLOADCMD=(
    'curl -sS --fail --max-time 120 -H "X-Auth: %s" '
    '--data-binary @$SOURCE http://$UPLOAD_PORT:8080/update' % lan_password()
))
