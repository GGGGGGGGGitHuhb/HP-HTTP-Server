"""Single charged R009 pure admission; compile syntax only, no build/workload."""
from pathlib import Path
import unittest
import test_r009_proc

if __name__=='__main__':
    root=Path(__file__).resolve().parent
    names=('proc_identity_v11.py','cleanup_v11.py','inner_identity_v11.py','localize_v11.py','watchdog_v11.py','observed_sample_v4.py','root_marker_launcher_v4.py','process_local_launcher_v4.py','prepare_r009_manifest.py')
    for name in names:compile((root/name).read_bytes(),str(root/name),'exec')
    suite=unittest.defaultTestLoader.loadTestsFromModule(test_r009_proc)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
