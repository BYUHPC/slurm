# Makefile.am for slurmd/common

AUTOMAKE_OPTIONS = foreign
CLEANFILES = core.*

AM_CPPFLAGS = -I$(top_srcdir)

# making a .la

noinst_LTLIBRARIES = libslurmd_common.la libslurmd_reverse_tree_math.la

libslurmd_common_la_SOURCES =    \
	core_spec_plugin.c core_spec_plugin.h \
	job_container_plugin.c job_container_plugin.h \
	proctrack.c proctrack.h \
	setproctitle.c setproctitle.h \
	slurmd_cgroup.c slurmd_cgroup.h \
	slurmstepd_init.c slurmstepd_init.h \
	run_script.c run_script.h \
	task_plugin.c task_plugin.h \
	set_oomadj.c set_oomadj.h \
	reverse_tree.h

libslurmd_reverse_tree_math_la_SOURCES = \
	reverse_tree_math.c \
	reverse_tree_math.h
