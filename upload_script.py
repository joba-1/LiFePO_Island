import configparser
import os

Import("env")

# ESP8266HTTPUpdateServer answers "Update Success!" on success.
# With an admin password in secrets.ini the upload authenticates; the credentials go to curl via
# a mode-600 config file, so they never show up in the process list.
# The ESP32 HTTPUpdateServer (core 3.x) also checks CSRF: the upload needs an Origin header
# equal to "http://" + Host, which browsers send and curl does not (without it: 401).
host = env.subst("$UPLOAD_PORT").split("/")[0]
origin = "-H 'Origin: http://%s'" % host
cmd = "curl -sS %s -F image=@$SOURCE $UPLOAD_PORT | tee /dev/stderr | grep -q 'Update Success'" % origin
path = os.path.join(env.subst("$PROJECT_DIR"), "secrets.ini")
if os.path.exists(path):
    cp = configparser.ConfigParser()
    cp.read(path)
    pw = cp.get("secrets", "admin_password", fallback="").strip()
    if pw:
        auth = os.path.join(env.subst("$BUILD_DIR"), "upload-auth.curl")
        os.makedirs(os.path.dirname(auth), exist_ok=True)
        fd = os.open(auth, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write('user = "admin:%s"\n' % pw.replace('"', '\\"'))
        cmd = "curl -sS -K %s %s -F image=@$SOURCE $UPLOAD_PORT | tee /dev/stderr | grep -q 'Update Success'" % (auth, origin)
env.Replace(UPLOADCMD=cmd)
