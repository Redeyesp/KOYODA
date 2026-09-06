#!/usr/bin/env python3
"""Host tests: actual worker with fake ESP calls, real pure animation policy."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
from generate_wifi_secrets import render

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise SystemExit("Host tests require GCC or Clang.")
    valid = [("KOYODA test", "test-only-password"), ("กอยอดะ", 'ab"cd\\$` ef12'),
             ("s"*32,"ab"*32), (" space "," space password "), ("x","a"*63)]
    invalid = [("","password"),("s"*33,"password"),("ก"*11,"password"),
               ("ok","short"),("ok","g"*64),("bad\nssid","password"),
               ("ok","pass\nword"),("ok","password\0"),("ok","รหัสผ่าน")]
    for ssid, password in invalid:
        try:
            render(ssid,password)
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid credential input accepted")
    with tempfile.TemporaryDirectory(prefix="koyoda-wifi-test-") as temp_name:
        temp=Path(temp_name)
        # Compile a copy so a user's private header in main/ cannot override
        # dummy test input (or accidentally become visible in test output).
        source=temp/"koyoda_wifi.c"
        shutil.copyfile(ROOT/"main/koyoda_wifi.c",source)
        flags=[cc,"-std=c11","-Wall","-Wextra","-Werror","-I"+str(temp),
               "-I"+str(ROOT/"tests/wifi_stubs"),"-I"+str(ROOT/"main")]
        executable=temp/"worker"
        def compile_worker(ssid: str, password: str, enabled: bool=True) -> None:
            (temp/"koyoda_wifi_secrets.h").write_text(render(ssid,password,enabled))
            subprocess.run(flags+[str(source),str(ROOT/"tests/test_wifi_worker.c"),"-o",str(executable)],check=True)
        def run(scenario: str) -> None:
            subprocess.run([str(executable),scenario],check=True,timeout=10)
        compile_worker(*valid[0])
        for scenario in ("healthy","missing_ap","wrong_password","connect_error",
                         "dhcp_timeout","recover","lost_ip","no_start","task_fail"):
            run(scenario)
        for stage in range(1,14):
            run(f"fail_{stage}")
        for ssid,password in valid[1:]:
            compile_worker(ssid,password)
            run("healthy")
        compile_worker("","",False)
        run("offline")
        print("PASS credentials: escaping, UTF-8 SSID, 32-byte SSID, 64-byte PSK, offline, invalid values")
        policy=temp/"policy"
        subprocess.run(flags+[str(ROOT/"tests/test_wifi_retry.c"),"-o",str(policy)],check=True)
        subprocess.run([str(policy)],check=True,timeout=10)
        # Bad input must fail without printing secrets or leaving an older header.
        private=temp/"stale.h"; private.write_text("stale")
        env=dict(os.environ,KOYODA_WIFI_SSID="dummy-ssid",KOYODA_WIFI_PASSWORD="short")
        result=subprocess.run(["python3",str(ROOT/"tools/generate_wifi_secrets.py"),"--output",str(private)],
                              env=env,capture_output=True,text=True)
        assert result.returncode != 0 and not private.exists()
        assert "dummy-ssid" not in result.stdout+result.stderr and "short" not in result.stdout+result.stderr
    print("All host tests passed. ESP-IDF build and physical-board test still required.")


if __name__ == "__main__":
    main()
