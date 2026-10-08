# R011 Builder first window command

Approved revision2; awaits Reviewer static admission and Leader controlled execution. Single run00220 includes7cleanup; no free compile/test. No proc rerun; runtime manifest not applicable to M2.

```sh
/usr/bin/env TMPDIR=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TEMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp XDG_CACHE_HOME=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/localize_v12.py --role builder --run-id run-r008-m2-pure-002 --kind selfcheck --seconds 20 --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 -- /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/run_r009_m2_checks_v4.py --role builder --output /home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/run-r008-m2-pure-002
```
