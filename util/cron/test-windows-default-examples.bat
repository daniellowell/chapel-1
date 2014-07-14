@ECHO OFF
REM Test default configuration against examples on windows.

REM WORKSPACE is always set inside our testing environment. If this is run
REM outside that environment, the caller is responsible for setting it.
IF "%WORKSPACE%"=="" GOTO ErrExit

REM NOTE: This is pretty messy, but it is the only way I could figure out how to
REM       get the correct environment setup and then invoke
REM       nightly.windows. (thomasvandoren, 2014-07-14)
c:\cygwin64\bin\bash -exc "export PATH='/usr/local/bin:/usr/bin:/cygdrive/c/windows/system32:/cygdrive/c/windows:/cygdrive/c/windows/System32/Wbem:/cygdrive/c/windows/System32/WindowsPowerShell/v1.0:/cygdrive/c/Program Files/SysinternalsSuite:/usr/bin:/cygdrive/c/emacs-24.3/bin' ; export CHPL_HOME=$WORKSPACE ; source $CHPL_HOME/util/cron/common.bash && $CHPL_HOME/util/cron/nightly.windows"
GOTO End

:ErrExit
ECHO "ERROR: WORKSPACE must be set in the environment"
EXIT 1
GOTO End

:End
