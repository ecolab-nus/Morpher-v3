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

