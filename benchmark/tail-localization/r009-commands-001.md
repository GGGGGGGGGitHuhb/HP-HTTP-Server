# R009 Builder 两次具名纯验证

两个入口各一次20秒，13工作+7清理。只有Reviewer静态准入后由Leader串行执行；Builder不自行运行。proc全/mock，无真实proc扫描/信号/负载；实际Reservation仅隔离TMP fixture账本。M2临时C单元有自身/proc身份与32MiB预分配，没有socket；ASanUBSanLSan首次窄提升，编译运行均纳入该20秒。静态manifest已生成，纯执行不隐藏创建manifest或重建冻结binary。

## run-r008-proc-pure-001

```sh
/usr/bin/env TMPDIR=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TEMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp XDG_CACHE_HOME=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/localize_v11.py --role builder --run-id run-r008-proc-pure-001 --kind selfcheck --seconds 20 --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 -- /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/run_r009_proc_pure.py
```

## run-r008-m2-pure-001

```sh
/usr/bin/env TMPDIR=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TEMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp XDG_CACHE_HOME=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/localize_v11.py --role builder --run-id run-r008-m2-pure-001 --kind selfcheck --seconds 20 --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 -- /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/run_r009_m2_checks.py --role builder --output /home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/run-r008-m2-pure-001
```
