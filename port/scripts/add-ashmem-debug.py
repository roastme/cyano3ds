#!/usr/bin/env python3
"""Temporary bring-up instrumentation for the vendored ashmem driver.

dalvik's first GC does

    ashmem_create_region("dalvik-heap-markstack", 700416)

and logs "Could not create %d-byte ashmem mark stack", but the same call
succeeds in the bring-up smoke test and the fd limit has been raised, so the
failure depends on the process state.  ashmem_create_region() is just
open("/dev/ashmem") + ASHMEM_SET_NAME + ASHMEM_SET_SIZE, so log the open and
every failing path with the errno; the kernel log will then say which one it is
(EMFILE/ENFILE from open, EINVAL from the ioctls, ENOMEM from shmem, ...).

Idempotent.  Remove this step once bring-up is done.
"""
import pathlib
import os
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
p = kd / "drivers/staging/android/ashmem.c"
s = p.read_text()

if "ASHMEM-BRINGUP-DEBUG" in s:
    print("    ashmem.c: instrumentation already present")
    raise SystemExit(0)

subs = [
    # open: log every open (so we can tell "open never happened" = VFS limit)
    ("\tasma = kmem_cache_zalloc(ashmem_area_cachep, GFP_KERNEL);\n"
     "\tif (!asma)\n"
     "\t\treturn -ENOMEM;\n",
     "\tasma = kmem_cache_zalloc(ashmem_area_cachep, GFP_KERNEL);\n"
     "\tif (!asma) {\n"
     "\t\tpr_err(\"ashmem[dbg]: open: alloc failed\\n\");\n"
     "\t\treturn -ENOMEM;\n"
     "\t}\n"
     "\tpr_info(\"ashmem[dbg]: open pid %d\\n\", task_pid_nr(current)); /* ASHMEM-BRINGUP-DEBUG */\n"),

    # set_name failures
    ("\tlen = strncpy_from_user(local_name, name, ASHMEM_NAME_LEN);\n"
     "\tif (len < 0)\n"
     "\t\treturn len;\n",
     "\tlen = strncpy_from_user(local_name, name, ASHMEM_NAME_LEN);\n"
     "\tif (len < 0) {\n"
     "\t\tpr_err(\"ashmem[dbg]: set_name: copy_from_user %d\\n\", len);\n"
     "\t\treturn len;\n"
     "\t}\n"),
    ("\tif (asma->file)\n"
     "\t\tret = -EINVAL;\n"
     "\telse\n"
     "\t\tstrcpy(asma->name + ASHMEM_NAME_PREFIX_LEN, local_name);\n",
     "\tif (asma->file) {\n"
     "\t\tpr_err(\"ashmem[dbg]: set_name: region already mapped\\n\");\n"
     "\t\tret = -EINVAL;\n"
     "\t} else\n"
     "\t\tstrcpy(asma->name + ASHMEM_NAME_PREFIX_LEN, local_name);\n"),

    # SET_SIZE failure
    ("\t\tif (!asma->file) {\n"
     "\t\t\tret = 0;\n"
     "\t\t\tasma->size = (size_t)arg;\n"
     "\t\t}\n",
     "\t\tif (!asma->file) {\n"
     "\t\t\tret = 0;\n"
     "\t\t\tasma->size = (size_t)arg;\n"
     "\t\t\tpr_info(\"ashmem[dbg]: SET_SIZE '%s' %lu pid %d\\n\",\n"
     "\t\t\t        asma->name + ASHMEM_NAME_PREFIX_LEN, arg,\n"
     "\t\t\t        task_pid_nr(current));\n"
     "\t\t} else {\n"
     "\t\t\tpr_err(\"ashmem[dbg]: SET_SIZE(%lu) on mapped region\\n\", arg);\n"
     "\t\t}\n"),

    # mmap failures
    ("\tif (!asma->size) {\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto out;\n"
     "\t}\n",
     "\tif (!asma->size) {\n"
     "\t\tpr_err(\"ashmem[dbg]: mmap: no SET_SIZE\\n\");\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto out;\n"
     "\t}\n"),
    ("\tif (vma->vm_end - vma->vm_start > PAGE_ALIGN(asma->size)) {\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto out;\n"
     "\t}\n",
     "\tif (vma->vm_end - vma->vm_start > PAGE_ALIGN(asma->size)) {\n"
     "\t\tpr_err(\"ashmem[dbg]: mmap: request %lu > size %lu\\n\",\n"
     "\t\t       vma->vm_end - vma->vm_start, (unsigned long)asma->size);\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto out;\n"
     "\t}\n"),
    ("\t\tvmfile = shmem_file_setup(name, asma->size, vma->vm_flags);\n"
     "\t\tif (IS_ERR(vmfile)) {\n"
     "\t\t\tret = PTR_ERR(vmfile);\n"
     "\t\t\tgoto out;\n"
     "\t\t}\n",
     "\t\tvmfile = shmem_file_setup(name, asma->size, vma->vm_flags);\n"
     "\t\tif (IS_ERR(vmfile)) {\n"
     "\t\t\tret = PTR_ERR(vmfile);\n"
     "\t\t\tpr_err(\"ashmem[dbg]: mmap: shmem_file_setup(%lu) = %d\\n\",\n"
     "\t\t\t       (unsigned long)asma->size, ret);\n"
     "\t\t\tgoto out;\n"
     "\t\t}\n"),
    ("\t\tret = shmem_zero_setup(vma);\n"
     "\t\tif (ret) {\n"
     "\t\t\tfput(asma->file);\n"
     "\t\t\tgoto out;\n"
     "\t\t}\n",
     "\t\tret = shmem_zero_setup(vma);\n"
     "\t\tif (ret) {\n"
     "\t\t\tpr_err(\"ashmem[dbg]: mmap: shmem_zero_setup = %d\\n\", ret);\n"
     "\t\t\tfput(asma->file);\n"
     "\t\t\tgoto out;\n"
     "\t\t}\n"),
]

for old, new in subs:
    if old not in s:
        sys.exit("error: ashmem.c: pattern not found:\n" + old[:80])
    s = s.replace(old, new, 1)

p.write_text(s)
print("    ashmem.c: bring-up failure instrumentation added")
