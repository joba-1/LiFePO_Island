# Pre-build: secrets from secrets.ini (untracked, mode 600), never from the repo.
#
#     [secrets]
#     admin_password = <password>
#     ; test bench only (envs with custom_bench_wifi = yes):
#     bench_wifi_ssid = <ssid>
#     bench_wifi_password = <password>
#
# admin_password protects /update, /reset, /ip and POST /api/netconfig, and
# the WiFiManager portal if it has >= 8 characters. Without it the firmware
# builds without the lock (/api/netstatus: "adminLock": false). The same
# password is used by upload_script.py for OTA uploads.
#
# The bench Wi-Fi lets a freshly erased test board join the network without
# going through the WiFiManager portal. BENCH_WIFI_SSID / BENCH_WIFI_PASS in the
# environment override secrets.ini.
import configparser
import os

Import("env")  # noqa: F821

path = os.path.join(env.subst("$PROJECT_DIR"), "secrets.ini")  # noqa: F821
cp = configparser.ConfigParser()
if os.path.exists(path):
    cp.read(path)


def secret(key):
    return cp.get("secrets", key, fallback="").strip()


password = secret("admin_password")
if password:
    env.Append(CPPDEFINES=[("ADMIN_PASSWORD", env.StringifyMacro(password))])  # noqa: F821
else:
    print("pio_secrets.py: no admin_password in secrets.ini - building WITHOUT admin lock")

if env.GetProjectOption("custom_bench_wifi", "no") == "yes":  # noqa: F821
    # environment wins (e.g. to put the bench into the island's network for diagnosis)
    ssid = os.environ.get("BENCH_WIFI_SSID") or secret("bench_wifi_ssid")
    wifi_pw = os.environ.get("BENCH_WIFI_PASS") if os.environ.get("BENCH_WIFI_SSID") else secret("bench_wifi_password")
    if ssid:
        env.Append(CPPDEFINES=[  # noqa: F821
            ("BENCH_WIFI_SSID", env.StringifyMacro(ssid)),  # noqa: F821
            ("BENCH_WIFI_PASS", env.StringifyMacro(wifi_pw)),  # noqa: F821
        ])
    else:
        print("pio_secrets.py: no bench_wifi_ssid in secrets.ini - WiFiManager portal needed on a new board")
