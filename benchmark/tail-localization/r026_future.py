"""One R026 anchor and literal future suffix; no directory creation."""
import os
from pathlib import Path
import stat
import sys

SUFFIX=('.cache','r022-benchmark-tests','run-r022-check-001')

def measure_future_benchmark(stage,role,scanner,fixture_link):
    if role not in ('builder','reviewer'):raise ValueError('R026 role')
    anchor=Path(stage)/role/'run-r018-build-001/build-output/source-B'
    target=anchor.joinpath(*SUFFIX)
    flags=os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC
    descriptors=[];held=[]
    signature=lambda value:(value.st_dev,value.st_ino,stat.S_IFMT(value.st_mode))
    def verify():
        for path,descriptor,expected in held:
            if signature(path.lstat())!=expected or signature(os.fstat(descriptor))!=expected:
                raise ValueError('R026 future ancestor identity changed')
    try:
        parent=os.open('/',flags);descriptors.append(parent)
        held.append((Path('/'),parent,signature(os.fstat(parent))))
        actual=Path('/')
        for part in anchor.parts[1:]:
            actual=actual/part;before=os.stat(part,dir_fd=parent,follow_symlinks=False)
            if not stat.S_ISDIR(before.st_mode):raise ValueError('R026 anchor ancestor directory required')
            child=os.open(part,flags,dir_fd=parent);descriptors.append(child)
            if signature(before)!=signature(os.fstat(child)):raise ValueError('R026 anchor identity changed')
            parent=child;held.append((actual,child,signature(before)))
        for part in SUFFIX:
            actual=actual/part
            try:before=os.stat(part,dir_fd=parent,follow_symlinks=False)
            except FileNotFoundError:
                verify()
                try:os.stat(part,dir_fd=parent,follow_symlinks=False)
                except FileNotFoundError:return {'bytes':0,'state':'not_created'}
                raise ValueError('R026 missing suffix changed during measurement')
            if not stat.S_ISDIR(before.st_mode):raise ValueError('R026 future suffix directory required')
            child=os.open(part,flags,dir_fd=parent);descriptors.append(child)
            if signature(before)!=signature(os.fstat(child)):raise ValueError('R026 suffix identity changed')
            parent=child;held.append((actual,child,signature(before)))
        verify()
        def classify(relative,path,status,link):
            if link is not None:return fixture_link(path,None,status,link)
            return status.st_size
        size=scanner(target,classify,lambda relative:bool(relative),root_open_path='/proc/self/fd/'+str(parent)+'/.')
        verify()
        return {'bytes':size,'state':'present'}
    finally:
        primary=sys.exc_info()[1];errors=[]
        for descriptor in reversed(descriptors):
            try:os.close(descriptor)
            except OSError as error:errors.append(error)
        if errors:
            if primary is not None:primary.add_note('R026 future FD close failures: '+repr(errors))
            else:raise errors[0]
