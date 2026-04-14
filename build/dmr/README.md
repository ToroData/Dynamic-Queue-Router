## Quick Start DMR

The following instructions are designed to get you up and running with DMR as fast as possible. More detailed instructions and advanced usage are available on the [Wiki](https://gitlab.bsc.es/accelcom/dmr/dmr/-/wikis/home).

### What is DMR?

DMR is a library that enables your MPI-based application to change its resource allocation during runtime. For example, you could launch your application with ten nodes and at some point in the execution scale down to one node, and then later grow to use twenty nodes.  The library handles most of the complex operations behind the scenes, so as a developer, your main focus is on implementing the logic required to redistribute your application's data when resources change.

### Modes of Operation

DMR offers two modes of operation. The first mode is one that connects to the system-default instance of Slurm. This mode is referred to as the DMR@Jobs mode. The second mode relies on a custom resource manager, Slurm4DMR, which runs as a nested instance inside of your system's resource manager under a fixed resource allocation assigned to it. This mode is referred to as DMR@Slurm. Note that the DMR@Slurm mode is currently not intended to run outside MareNostrum5.

### Prerequisite Setup Instructions

If you are on MareNostrum5, most of the dependencies that you need are already installed. You can easily get them and set the required environment variables by loading the modules below.

```
module use /apps/GPP/DMR/dmr-modules
module load openpmix-for-dmr
module load prrte-for-dmr
module load openmpi-for-dmr
module load dlb-for-dmr # Optional; enables TALP-based policies
```

If you are not on MareNostrum5, or do not want to use the preinstalled versions, you can install the dependencies manually. DMR relies on Open MPI with PRRTE to work and the DMR@Slurm mode requires TALP, which is part of DLB. Instructions are available on the Wiki to install the proper versions: 
* [External Dependency Installation General Instructions](https://gitlab.bsc.es/accelcom/dmr/dmr/-/wikis/External-Dependency-Installation-General-Instructions)
* [External Dependency Installation Process for MareNostrum5](https://gitlab.bsc.es/accelcom/dmr/dmr/-/wikis/External-Dependency-Installation-Process-for-MareNostrum5)

If you already have Open MPI installed on your system, you are still advised to follow these instructions as <b>your pre-existing installation of Open MPI is unlikely to be compatible with DMR</b>, because it relies on (very) recently added features of PRRTE.

### Connecting to Slurm

The provided cmake script should already identify your system's default Slurm installation by default. If you are having issues, you can try to follow the manual instructions provided [here](https://gitlab.bsc.es/accelcom/dmr/dmr/-/wikis/Connecting-to-System-Default-Slurm).

If you want to run inside a fixed allocation using Slurm4DMR, ensure you have all the submodules of DMR (``git submodule update --init --recursive``) and then navigate to ``tools/slurm``. Follow the instructions in the README to install Slurm4DMR. To compile against your installation, set the environment variable SLUMR4DMR to 1 (``export SLURM4DMR=1``) and ensure ``SLURM_ROOT`` points to the location where you installed Slurm4DMR.

### 1. Compilation

The compilation process requires cmake. On MareNostrum5, type ``module load cmake``. You do not need this module after compilation.

First, note down some details about your Slurm account and your desired default configuration. You can set them before running the cmake command. You might want to set the following:

* ``DMR_NODES_IN_EXPAND``: Number of nodes in each expander job, if not overriden. Defaults to 1.
* ``DMR_PROCS_PER_NODE``: Number of processes to spawn per node which is added, if not overriden. Defaults to 1.

You can override these in the environment by exporting them when you run with DMR, but the compiled values will be used as defaults. Set these variables when you use cmake, for example like this:

``cmake -DDMR_NODES_IN_EXPAND=10 -DDMR_PROCS_PER_NODE=112 ..``

You can also omit one or all options, in which case the default will be used. The cmake process will provide you with output that will let you know the options have been set. Variables that are exported into the environment will also be detected by the cmake script, unless overriden by the cmake command/cache.

Now you are ready to compile:

```
# Ensure you have cmake available (On MareNostrum5: module load cmake)

mkdir build
cd build
cmake ..
make
make install
```
By default, this will place your compiled files in the DMR root folder in a folder called ``lib``. You can adjust the installation folder using ``CMAKE_INSTALL_PREFIX``.

You should identify the DMR base folder where you have the compiled ``.so`` files and add them to LD_LIBRARY_PATH, too. First, for convenience, you can export the environment variable ``DMR_PATH`` to the place where the DMR code is. Then, add it to LD_LIBRARY_PATH. Optionally, also add ``$DMR_PATH/scripts`` to PATH, so you can easily run the DMR wrapper.

```
export DMR_PATH=/path/to/dmr
export LD_LIBRARY_PATH=$DMR_PATH/lib:$LD_LIBRARY_PATH
export PATH=$DMR_PATH/scripts:$PATH
```

### 2. Building a Hello World DMR application

Now, you will have compiled the DMR library. The next step is to run some code using it. Navigate to the folder ``examples`` to see example DMR applications (if you cannot see it, ensure you have fetched the submodules). In this folder, you will find ``hello-world`` containing the C code ``hello-world.c``. It comes with a sample Makefile and batch job, which you can use as a template for your own programs.

In the ``hello-world.c`` program, you will see that the macro ``DMR_AUTO(...)`` is used in the main function. This macro simplifies the use of DMR, as you only need to provide it with the required functions to call when checkpointing/restarting and it will handle the rest. It supports any DMR function which returns a DMRAction. With ``DMR_AUTO(..)`` the three main cases you will want to handle explicitly are:

* Initialization with ``dmr_init(argc, argv)``
* Progress check / reconfiguration with ``dmr_check(suggestion)``
* Data cleanup with ``dmr_finalize()``

You are required to initialize DMR before using most other functionality, through a call to dmr_init. This, as in hello-world.c, can be done like this:

``DMR_AUTO(dmr_init(argc, argv), (void)NULL, restart(arguments), (void)NULL);``

This will call the function ``dmr_init(argc, argv)`` first and then, if necessary, call ``restart(arguments)`` to re-load data after the program has restarted or reconfigured. Notice how some arguments are left blank using ``(void)NULL``. This is because they are not needed when initializing.

The dmr_check step is how DMR makes decisions about how to reconfigure. The function takes an argument of type DMRSuggestion, see dmr.h, which tells DMR which action to take. Using DMR_AUTO, the call looks something like this:

``DMR_AUTO(dmr_check(SUGGESTION), checkpoint(arguments), (void)NULL, finalize(arguments));``

Where ``SUGGESTION`` is a DMRSuggestion which can take any of the following values:

| **DMRSuggestion**     | **Result**                                                                                                                                                                   | **Compile-Time  Requirements**   |
|-----------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------|
| SHOULD_EXPAND         | Add a configured number of additional resources                                                                                                                               | None                             |
| SHOULD_SHRINK         | Remove a configured number of resources                                                                                                                                       | None                             |
| SHOULD_STAY           | Continue execution without reconfiguring                                                                                                                                      | None                             |
| TEST_POLICY           | Expand up to set maximum number of resources by doubling, then shrink to set minimum number                                                                                   | None                             |
| TALP_POLICY           | Expand or shrink to try to reach a communication efficiency of DMR_TALP_TARGET_CE                                                                                             | TALP               |
| SLURM4DMR_TEST_POLICY | Expand up to set maximum number of resources by multiplying by a set set factor, then shrink to set minimum number. Consider the cluster status in reconfiguration decisions. | Slurm4DMR          |
| SLURM4DMR_TALP_POLICY | Expand or shrink to try to reach a communication efficiency of DMR_TALP_TARGET_CE. Consider the cluster status in reconfiguration decisions.                                  | Slurm4DMR and TALP |
| SLURM4DMR_STEP_POLICY | Try to target an ideal node count, accepting changes between the minimum and maximum count depending on the cluster status.                                                             | Slurm4DMR          |

Note that it is <b>very important</b> that all processes agree on the suggestion at all times; otherwise they will deadlock. It is up to the user to ensure the suggestion is synchronized. Note that in DMR@Jobs mode, DMR_AUTO will not block while a resource request is pending, so your application can continue to run while waiting for resources. In both modes, you can toggle this behavior using the environment variable DMR_BLOCKING_REQ at runtime or compile-time. Once a reconfiguration has been suggested, a different suggestion will not alter the course of the reconfiguration; though a pending expansion can be cancelled using ``dmr_cancel_expansion()``.

The checkpoint(arguments) function is called whenever a reconfiguration occurs. In this case, the restart argument can be left blank with ``(void)NULL``, too, as it will not be needed. The last ``finalize(arguments)`` function is an optional function that DMR will call before a given process exits due to a reconfiguration.

Finally, a function to disable DMR functionality and clean up the internals is provided with ``dmr_finalize``. You can wrap this in a ``DMR_AUTO`` if you want to, leaving all the other arguments ``(void)NULL``, but you do not have to, because it never requires any further actions to be performed. Note that you do not need to explicitly deal with calling this function during reconfigurations, as it will be done automatically with ``DMR_AUTO(...)``. However, once your program reaches a point where it will no longer be using DMR, you can insert it.

To compile ``hello-world.c``, ensure that ``DMR_PATH`` is defined (see first section), then type ``make helloJobs`` (DMR@Jobs approach) or ``make helloSlurm`` (DMR@Slurm approach). 

### 3. Running with DMR

To run your application with DMR, you will need to use the provided ``dmr_wrapper``. You will also need to use one of ``mpirun``, ``mpiexec``, or ``prterun`` to launch. Since DMR requires PRRTE to run outside of managed allocation mode, you will need to specify the initial hosts to run on and their slots explicitly. The ``batch_submit.sbatch`` file in ``examples/hello-world-dmr`` contains an example of how to do this. The command to launch looks something like this (assuming ``$DMR_PATH/scripts`` is on ``PATH``):

``dmr_wrapper mpirun --host $HOSTS_WITH_COUNTS ./hello_world``

This uses standard OpenMPI syntax to launch the program (see the OpenMPI documentation), passing it all to the dmr_wrapper.

Edit the template batch job file with your desired configuration and your account details. You are now ready to submit your first DMR batch job:

``sbatch batch_submit.sbatch`` (DMR@Jobs approach)  
or ``sbatch start_custom_slurm.sh`` (DMR@Slurm approach)

That's it! Your DMR job is now running. For more advanced usage, and correctness testing, check out the example file in ``examples/distributed-dataset-sleep``. It passes data between iterations and validates the correctness of this data at each reconfiguration.