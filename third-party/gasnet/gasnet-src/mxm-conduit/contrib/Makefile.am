## Process this file with automake to produce Makefile.in       -*- makefile -*-
# Description: Makefile for GASNet MXM spawner
# Terms of use are as specified in mxm-conduit/license.txt

AUTOMAKE_OPTIONS = foreign 1.4

# Use of the MPI spawning support is optional
if MXM_BOOTSTRAP_MPI
mpi_target_pl = gasnetrun_mxm-mpi.pl
$(mpi_target_pl): $(top_srcdir)/mpi-conduit/contrib/gasnetrun_mpi.pl
	cp -f "$?" $@
else
mpi_target_pl =
endif

target = gasnetrun_mxm

# Need a bin_DATA rule, but automake disallows that.  So, fake it.
dotpldir = $(bindir)

# Make sure the perl script and shell wrapper are always current locally.
# This ensures we can run from the build directory if needed.
$(top_builddir)/other/perlstart: $(top_srcdir)/other/perlstart.in
	@cd $(top_builddir)/other && $(MAKE) perlstart
$(top_builddir)/other/mpirun_common: $(top_srcdir)/other/mpirun_common.in
	@cd $(top_builddir)/other && $(MAKE) mpirun_common
$(target): $(top_builddir)/other/perlstart $(top_builddir)/other/mpirun_common
$(target).pl: $(top_srcdir)/ibv-conduit/contrib/gasnetrun_ibv.pl
	cp -f "$?" $@
all-local: $(target) $(target).pl $(mpi_target_pl)

DISTCLEANFILES = $(target) $(target).pl $(mpi_target_pl)

# Conditionally install the .pl and its wrapper
if USE_MXM_CONDUIT
scripts = $(target)
data = $(target).pl $(mpi_target_pl)
else
scripts =
data =
endif
bin_SCRIPTS = $(scripts)
dotpl_DATA = $(data)

.PHONY: force
force:

