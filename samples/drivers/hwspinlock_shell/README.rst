.. zephyr:code-sample:: hwspinlock_shell
   :name: HWSPINLOCK shell
   :relevant-api: hwspinlock_interface

Overview
********

This sample enables the ``hwspinlock`` shell command (see
:ref:`hwspinlock_api`) for interactive testing.

It is useful when the remote side is not running Zephyr (e.g. another OS or a
firmware monitor). The shell provides commands to list hwspinlock controllers,
select a lock, and lock/unlock it.

Building
********

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/hwspinlock_shell
   :board: imx95_evk/mimx9596/m7
   :goals: build

Running
*******

On the UART shell:

.. code-block:: console

   uart:~$ hwspinlock select <dev> <id>
   uart:~$ hwspinlock status
   uart:~$ hwspinlock trylock
   uart:~$ hwspinlock lock
   uart:~$ hwspinlock unlock
