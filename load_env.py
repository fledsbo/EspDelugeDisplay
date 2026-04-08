Import("env")
import os

env_file = os.path.join(env.subst("$PROJECT_DIR"), ".env")
if os.path.exists(env_file):
    with open(env_file) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                env.Append(CPPDEFINES=[(key.strip(), env.StringifyMacro(value.strip()))])
else:
    print("WARNING: .env file not found — WiFi credentials not set")
    print("  Create a .env file with WIFI_SSID and WIFI_PASSWORD")
