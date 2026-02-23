import glob
import os
import shutil


def remove_glob(cwd, pattern):
  for path in glob.glob(os.path.join(cwd, pattern)):
    if os.path.isdir(path):
      shutil.rmtree(path)
    else:
      os.remove(path)


def copy_file(src, dst_dir):
  shutil.copy2(src, dst_dir)


def copy_glob(cwd, pattern, dst_dir, required=False):
  matches = glob.glob(os.path.join(cwd, pattern))
  if required and not matches:
    raise RuntimeError("No files matched pattern: %s" % pattern)
  for src in matches:
    shutil.copy2(src, dst_dir)

