# Slurm 14.03.5 + LEVEL_BASED

This Slurm fork contains the code for the LEVEL_BASED prioritization mechanism.  It has been submitted for inclusion in Slurm 14.11.

### Documentation

[LEVEL_BASED Documentation](https://marylou.byu.edu/documentation/slurm/level_based.php)

[Problems with Existing Slurm Prioritization Methods](http://tech.ryancox.net/2014/06/problems-with-slurm-prioritization.html)

[Extra information about LEVEL_BASED](http://tech.ryancox.net/2014/06/slurm-levelbased.html)

### Version

Current version:  14.03.5 + LEVEL_BASED code.

Original version:  The *level_based* branch was originally branched from *slurm-14.03* when 14.03.4-2 was tagged and has since had updates merged in.

Future changes from *slurm-14.03* will likely be merged when minor versions are released by SchedMD.  Contact us if you want to be informed of future releases or check back a few days after a new Slurm minor release.

### Download
```
git clone -b level_based http://github.com/BYUHPC/slurm
```

or

```
git clone http://github.com/BYUHPC/slurm
git checkout level_based
```

### Installation

Nothing special is required to compile this version.  See http://slurm.schedmd.com/quickstart_admin.html

Follow the standard [Slurm upgrade procedure](http://slurm.schedmd.com/quickstart_admin.html#upgrade), **especially the ordering in the numbered list** in that section.

BYU upgraded live from standard Slurm 14.03 to this fork and did not have any problems.  However, there is a potential for issues since some data structures used in RPC calls now have additional fields.  This is unlikely to be a problem if you follow the standard [upgrade procedure](http://slurm.schedmd.com/quickstart_admin.html#upgrade) but you should try it out on a test system if possible.

### Authors

LEVEL_BASED was co-authored by [Ryan Cox](http://tech.ryancox.net) and [Levi Morrison](https://github.com/morrisonlevi) at [Brigham Young University](http://byu.edu)'s [Fulton Supercomputing Lab](https://marylou.byu.edu).

### Contact

[Contact Ryan or Levi](https://marylou.byu.edu/contact) with any questions and comments.  We would love to hear your feedback, so let us know if you try it out.
