  #!/usr/bin/env python
import sys
import os
import os.path
import shutil
import subprocess

from os import O_TRUNC, listdir
from os.path import isfile, join
import re
import numpy as np
from tqdm import tqdm
import yaml
from time import sleep
############################################
# Directory Structure:
# Morpher Home:
#     -Morpher_DFG_Generator
#     -Morpher_CGRA_Mapper
#     -hycube_simulator




def main(csource, function, config="config/default_config.yaml", runmode='runall'):

  print(r"""
    __  ___                 __                 ________________  ___       ____            _                ______                                             __  
   /  |/  /___  _________  / /_  ___  _____   / ____/ ____/ __ \/   |     / __ \___  _____(_)___ _____     / ____/________ _____ ___  ___ _      ______  _____/ /__
  / /|_/ / __ \/ ___/ __ \/ __ \/ _ \/ ___/  / /   / / __/ /_/ / /| |    / / / / _ \/ ___/ / __ `/ __ \   / /_  / ___/ __ `/ __ `__ \/ _ \ | /| / / __ \/ ___/ //_/
 / /  / / /_/ / /  / /_/ / / / /  __/ /     / /___/ /_/ / _, _/ ___ |   / /_/ /  __(__  ) / /_/ / / / /  / __/ / /  / /_/ / / / / / /  __/ |/ |/ / /_/ / /  / ,<   
/_/  /_/\____/_/  / .___/_/ /_/\___/_/      \____/\____/_/ |_/_/  |_|  /_____/\___/____/_/\__, /_/ /_/  /_/   /_/   \__,_/_/ /_/ /_/\___/|__/|__/\____/_/  /_/|_|  
                 /_/                                                                     /____/                                                                    
    """)

  # https://www.ascii-art-generator.org/
  # Font: slant

  stream = open(config, 'r')
  config_dict = yaml.safe_load(stream)
  # for key, value in config_dict.items():
  #       print (key + " : " + str(value))

  json_arch_before_memupdate = config_dict["json_arch_before_memupdate"]
  json_arch = config_dict["json_arch"]
  mapper_subfolder = config_dict["mapper_subfolder"]
  init_II = config_dict["init_II"]
  ydim = config_dict["ydim"]
  xdim = config_dict["xdim"]
  numberofbanks = config_dict["numberofbanks"]
  banksize = config_dict["banksize"] #8192*2 #8192*2
  totalsize = numberofbanks*banksize
  max_test_samples = config_dict["max_test_samples"]
  mapping_method = config_dict["mapping_method"]
  llvm_debug_type = config_dict["llvm_debug_type"] #'nothing' #'instrumentation'
  
  morpher_light = True if config_dict["morpher_light"].lower() == "yes" else False
  kernel = function
  appfolder, csourcefile = csource.rsplit('/', 1)

  # if not 'MORPHER_HOME' in os.environ:
  #   raise Exception('Set MORPHER_HOME directory as an environment variable (Ex: export MORPHER_HOME=/home/dmd/Workplace/Morphor/github_ecolab_repos)')

  MORPHER_HOME = os.getcwd() #os.getenv('MORPHER_HOME')
  DFG_GEN_HOME = MORPHER_HOME + '/Morpher_DFG_Generator'
  MAPPER_HOME = MORPHER_HOME + '/Morpher_CGRA_Mapper'
  SIMULATOR_HOME = MORPHER_HOME + '/hycube_simulator'

  DFG_GEN_KERNEL = DFG_GEN_HOME + '/benchmarks/'+appfolder+'/'
  MAPPER_KERNEL = MAPPER_HOME + '/benchmarks/'+mapper_subfolder+'/'+appfolder+'/'
  SIMULATOR_KERNEL =SIMULATOR_HOME + '/benchmarks/'+appfolder+'/'

  # my_mkdir(DFG_GEN_KERNEL)
  my_mkdir(MAPPER_KERNEL)
  my_mkdir(SIMULATOR_KERNEL)
  
  MEM_TRACE = DFG_GEN_KERNEL + 'memtraces'
  
  my_mkdir(MEM_TRACE)
  # print('\n\n############## Morpher CGRA Framework #################\n')
  print('\n Kernel: %s \n C source: %s/benchmarks/%s \n CGRA arch: %s/json_arch/%s \n Config: %s\n Run mode: %s\n'% (kernel, DFG_GEN_HOME,csource, MAPPER_HOME, json_arch, config, runmode))


##############################################################################################################################################
  if runmode == 'runall' or runmode == 'dfg_gen_only':
    print('-----Running Morpher_DFG_Generator-----\n')
    os.chdir(DFG_GEN_KERNEL)
  
    
    subprocess.run('rm memtraces/*', shell=True, check=False)
    subprocess.run('rm *.xml', shell=True, check=False)
    subprocess.run('rm *.dot', shell=True, check=False)
    print('Generating IR..\n')
    subprocess.run('clang -D CGRA_COMPILER -target i386-unknown-linux-gnu -Wno-implicit-function-declaration -Wno-format -Wno-main-return-type -c -emit-llvm -O2 -fno-tree-vectorize -fno-unroll-loops %s -S -o %s.ll'%(csourcefile,kernel), shell=True, check=True)

    print('Optimizing IR..\n')
    subprocess.run('opt -gvn -mem2reg -memdep -memcpyopt -lcssa -loop-simplify -licm -loop-deletion -indvars -simplifycfg -mergereturn -indvars  %s.ll -S -o %s_opt.ll' % (kernel,kernel), shell=True, check=True)

    print('Generating DFG (%s_PartPredDFG.xml/dot) and data layout (%s_mem_alloc.txt)..\n' % (kernel,kernel))
    subprocess.run('opt -load %s/build/src/libdfggenPass.so -fn %s -nobanks %d -banksize %d -skeleton %s_opt.ll -S -o %s_opt_instrument.ll' % (DFG_GEN_HOME,kernel,numberofbanks,banksize,kernel,kernel), shell=True, check=True)

    subprocess.run('dot -Tpdf %s_PartPredDFG.dot -o %s_PartPredDFG.pdf' % (kernel,kernel), shell=True, check=True)
    subprocess.run('cp '+kernel+'_PartPredDFG.xml '+ MAPPER_KERNEL, shell=True, check=True)
    # Copy mem_alloc early so the mapper can proceed even if instrumentation fails.
    subprocess.run('cp '+kernel+'_mem_alloc.txt '+MAPPER_KERNEL, shell=True, check=True)
    subprocess.run('rm *.log', shell=True, check=False)

    #if json_arch == 'hycube_original_mem.json':
    ###
    if morpher_light==False:
      print('\nCode instrumentation..\n')
      subprocess.run('clang -target i386-unknown-linux-gnu -c -emit-llvm -S %s/src/instrumentation/instrumentation.cpp -o instrumentation.ll' % DFG_GEN_HOME, shell=True, check=True)
      if kernel=='kernel_symm':
          subprocess.run('clang -D CGRA_COMPILER -target i386-unknown-linux-gnu -c -emit-llvm -O2 -fno-tree-vectorize -fno-inline -fno-unroll-loops polybench.c -S -o polybench.ll', shell=True, check=True)
          subprocess.run('llvm-link %s_opt_instrument.ll instrumentation.ll polybench.ll -o final.ll' % (kernel), shell=True, check=True)
      else:
          subprocess.run('llvm-link %s_opt_instrument.ll instrumentation.ll -o final.ll' % (kernel), shell=True, check=True)

      subprocess.run('llc -filetype=obj final.ll -o final.o', shell=True, check=True)
      subprocess.run('clang++ -m32 final.o -o final', shell=True, check=True)

      print('Running instrumented code to generate the data memory content (memtraces/%s_trace_x.txt)..\n' % kernel)
      result = subprocess.run('./final 1> final_log.txt 2> final_err_log.txt', shell=True, check=False)
      if result.returncode != 0:
        print('Warning: instrumented binary ./final exited with code %d; simulation traces will not be available.' % result.returncode)
      else:
        subprocess.run('cp memtraces/'+kernel+'_trace_0.txt '+SIMULATOR_KERNEL, shell=True, check=True)
        subprocess.run('cp '+kernel+'_mem_alloc.txt '+SIMULATOR_KERNEL, shell=True, check=True)
    ###
    subprocess.run('rm *.ll', shell=True, check=False)
 
  
##############################################################################################################################################
  if runmode == 'runall' or runmode == 'mapper_only':
    print('\n-----Running Morpher_CGRA_Mapper-----\n')
    os.chdir(MAPPER_KERNEL)
    
    light_mapper = ""
    if morpher_light==True:
       light_mapper = "--morpher_light"
    # os.system('rm *.bin')  
    if '_mem' in json_arch:
      print('\nUpdating memory allocation..\n')
      subprocess.run('python %s/update_mem_alloc.py %s/json_arch/%s %s_mem_alloc.txt %d %d %s' % (MAPPER_HOME,MAPPER_HOME, json_arch_before_memupdate,kernel,banksize,numberofbanks, json_arch), shell=True, check=True)
      subprocess.run('%s/build/src/cgra_xml_mapper -d %s_PartPredDFG.xml -x %d -y %d -j %s -i %d -t HyCUBE_4REG -m %d %s' % (MAPPER_HOME, kernel,xdim,ydim,json_arch, init_II, mapping_method, light_mapper), shell=True, check=True)
      print('%s/build/src/cgra_xml_mapper -d %s_PartPredDFG.xml -x %d -y %d -j %s -i %d -t HyCUBE_4REG -m %d %s' % (MAPPER_HOME, kernel,xdim,ydim,json_arch, init_II, mapping_method, light_mapper))
      os.chdir(SIMULATOR_KERNEL)
      subprocess.run('rm *.bin', shell=True, check=False)

      os.chdir(MAPPER_KERNEL)
      subprocess.run('cp *.bin '+ SIMULATOR_KERNEL, shell=True, check=True)
    else:
      subprocess.run('%s/build/src/cgra_xml_mapper -d %s_PartPredDFG.xml -x %d -y %d -j %s/json_arch/%s -i %d -m %d %s' % (MAPPER_HOME,kernel,xdim,ydim,MAPPER_HOME, json_arch, init_II, mapping_method, light_mapper), shell=True, check=True)
      print('%s/build/src/cgra_xml_mapper -d %s_PartPredDFG.xml -x %d -y %d -j %s/json_arch/%s -i %d -m %d %s' % (MAPPER_HOME,kernel,xdim,ydim,MAPPER_HOME, json_arch, init_II, mapping_method, light_mapper))
     
  

##############################################################################################################################################
  if ((runmode == 'runall' or runmode == 'sim_only') and not morpher_light):
    print('\n-----Running hycube_simulator-----\n')
    os.chdir(SIMULATOR_KERNEL) 
    files = [f for f in listdir(MEM_TRACE) if isfile(join(MEM_TRACE, f)) and re.match(kernel+"_trace_[0-9]*\.txt", f)]
    # print("Number of memtraces to be verified: "+str(len(files)))
    if len(files) > max_test_samples:
      samplefiles = np.random.choice(files, size=max_test_samples, replace=False)
    else:
      samplefiles = np.random.choice(files, size=len(files), replace=False)

    matches = 0
    mismatches = 0
    for file in tqdm(samplefiles):
        # os.system('cp '+join(MEM_TRACE, file)+' '+SIMULATOR_KERNEL)
        command = SIMULATOR_HOME+'/src/build/hycube_simulator -x '+str(xdim)+' -y '+str(ydim)+ ' -c '+SIMULATOR_KERNEL+'*.bin -d '+join(MEM_TRACE, file)+' -a '+SIMULATOR_KERNEL+kernel+'_mem_alloc.txt -m ' + str(totalsize)
        print(command)
        subprocess.run(command, shell=True, check=True)
        f = open("sim_result.txt", "r")
        sim_result = f.read()
        sim_result_ = sim_result.split(",")
        sim_result__ = [int(e) for e in sim_result_]
        matches_, mismatches_ = sim_result__
        matches = matches + matches_
        mismatches = mismatches + mismatches_
    print('Matches: %d Mismatches: %d' %(matches,mismatches))  #   
    if mismatches == 0:
     print('Simulation test passed!!!')

  

def my_mkdir(dir):
    os.makedirs(dir, exist_ok=True)

if __name__ == '__main__':
  csource = sys.argv[1]
  function = sys.argv[2]
  config = sys.argv[3] if len(sys.argv) >= 4 else 'config/default_config.yaml'
  runmode = sys.argv[4] if len(sys.argv) >= 5 else 'runall'
  main(csource, function, config, runmode)

