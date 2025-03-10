import errno
import json
import logging
import traceback
import threading

from mgr_module import MgrModule
import orchestrator

from .fs.volume import VolumeClient
from .fs.nfs import NFSCluster, FSExport

log = logging.getLogger(__name__)

class VolumesInfoWrapper():
    def __init__(self, f, context):
        self.f = f
        self.context = context
    def __enter__(self):
        log.info("Starting {}".format(self.context))
    def __exit__(self, exc_type, exc_value, tb):
        if exc_type is not None:
            log.error("Failed {}:\n{}".format(self.context, "".join(traceback.format_exception(exc_type, exc_value, tb))))
        else:
            log.info("Finishing {}".format(self.context))

def mgr_cmd_wrap(f):
    def wrap(self, inbuf, cmd):
        astr = []
        for k in cmd:
            astr.append("{}:{}".format(k, cmd[k]))
        context = "{}({}) < \"{}\"".format(f.__name__, ", ".join(astr), inbuf)
        with VolumesInfoWrapper(f, context):
            return f(self, inbuf, cmd)
    return wrap

class Module(orchestrator.OrchestratorClientMixin, MgrModule):
    COMMANDS = [
        {
            'cmd': 'fs volume ls',
            'desc': "List volumes",
            'perm': 'r'
        },
        {
            'cmd': 'fs volume create '
                   'name=name,type=CephString '
                   'name=placement,type=CephString,req=false ',
            'desc': "Create a CephFS volume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs volume rm '
                   'name=vol_name,type=CephString '
                   'name=yes-i-really-mean-it,type=CephString,req=false ',
            'desc': "Delete a FS volume by passing --yes-i-really-mean-it flag",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolumegroup ls '
            'name=vol_name,type=CephString ',
            'desc': "List subvolumegroups",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolumegroup create '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString '
                   'name=pool_layout,type=CephString,req=false '
                   'name=uid,type=CephInt,req=false '
                   'name=gid,type=CephInt,req=false '
                   'name=mode,type=CephString,req=false ',
            'desc': "Create a CephFS subvolume group in a volume, and optionally, "
                    "with a specific data pool layout, and a specific numeric mode",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolumegroup rm '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString '
                   'name=force,type=CephBool,req=false ',
            'desc': "Delete a CephFS subvolume group in a volume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume ls '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "List subvolumes",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolume create '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=size,type=CephInt,req=false '
                   'name=group_name,type=CephString,req=false '
                   'name=pool_layout,type=CephString,req=false '
                   'name=uid,type=CephInt,req=false '
                   'name=gid,type=CephInt,req=false '
                   'name=mode,type=CephString,req=false '
                   'name=namespace_isolated,type=CephBool,req=false ',
            'desc': "Create a CephFS subvolume in a volume, and optionally, "
                    "with a specific size (in bytes), a specific data pool layout, "
                    "a specific mode, in a specific subvolume group and in separate "
                    "RADOS namespace",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume rm '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=group_name,type=CephString,req=false '
                   'name=force,type=CephBool,req=false '
                   'name=retain_snapshots,type=CephBool,req=false ',
            'desc': "Delete a CephFS subvolume in a volume, and optionally, "
                    "in a specific subvolume group, force deleting a cancelled or failed "
                    "clone, and retaining existing subvolume snapshots",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume authorize '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=auth_id,type=CephString '
                   'name=group_name,type=CephString,req=false '
                   'name=access_level,type=CephString,req=false '
                   'name=tenant_id,type=CephString,req=false '
                   'name=allow_existing_id,type=CephBool,req=false ',
            'desc': "Allow a cephx auth ID access to a subvolume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume deauthorize '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=auth_id,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Deny a cephx auth ID access to a subvolume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume authorized_list '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "List auth IDs that have access to a subvolume",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolume evict '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=auth_id,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Evict clients based on auth IDs and subvolume mounted",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolumegroup getpath '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString ',
            'desc': "Get the mountpath of a CephFS subvolume group in a volume",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolume getpath '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Get the mountpath of a CephFS subvolume in a volume, "
                    "and optionally, in a specific subvolume group",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume info '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Get the metadata of a CephFS subvolume in a volume, "
                    "and optionally, in a specific subvolume group",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolumegroup pin'
                   ' name=vol_name,type=CephString'
                   ' name=group_name,type=CephString,req=true'
                   ' name=pin_type,type=CephChoices,strings=export|distributed|random'
                   ' name=pin_setting,type=CephString,req=true',
            'desc': "Set MDS pinning policy for subvolumegroup",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolumegroup snapshot ls '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString ',
            'desc': "List subvolumegroup snapshots",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolumegroup snapshot create '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString '
                   'name=snap_name,type=CephString ',
            'desc': "Create a snapshot of a CephFS subvolume group in a volume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolumegroup snapshot rm '
                   'name=vol_name,type=CephString '
                   'name=group_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=force,type=CephBool,req=false ',
                   'desc': "Delete a snapshot of a CephFS subvolume group in a volume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume snapshot ls '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "List subvolume snapshots",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolume snapshot create '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Create a snapshot of a CephFS subvolume in a volume, "
                    "and optionally, in a specific subvolume group",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume snapshot info '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Get the metadata of a CephFS subvolume snapshot "
                    "and optionally, in a specific subvolume group",
            'perm': 'r'
        },
        {
            'cmd': 'fs subvolume snapshot rm '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=group_name,type=CephString,req=false '
                   'name=force,type=CephBool,req=false ',
            'desc': "Delete a snapshot of a CephFS subvolume in a volume, "
                    "and optionally, in a specific subvolume group",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume resize '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=new_size,type=CephString,req=true '
                   'name=group_name,type=CephString,req=false '
                   'name=no_shrink,type=CephBool,req=false ',
            'desc': "Resize a CephFS subvolume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume pin'
                   ' name=vol_name,type=CephString'
                   ' name=sub_name,type=CephString'
                   ' name=pin_type,type=CephChoices,strings=export|distributed|random'
                   ' name=pin_setting,type=CephString,req=true'
                   ' name=group_name,type=CephString,req=false',
            'desc': "Set MDS pinning policy for subvolume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume snapshot protect '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "(deprecated) Protect snapshot of a CephFS subvolume in a volume, "
                    "and optionally, in a specific subvolume group",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume snapshot unprotect '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "(deprecated) Unprotect a snapshot of a CephFS subvolume in a volume, "
                    "and optionally, in a specific subvolume group",
            'perm': 'rw'
        },
        {
            'cmd': 'fs subvolume snapshot clone '
                   'name=vol_name,type=CephString '
                   'name=sub_name,type=CephString '
                   'name=snap_name,type=CephString '
                   'name=target_sub_name,type=CephString '
                   'name=pool_layout,type=CephString,req=false '
                   'name=group_name,type=CephString,req=false '
                   'name=target_group_name,type=CephString,req=false ',
            'desc': "Clone a snapshot to target subvolume",
            'perm': 'rw'
        },
        {
            'cmd': 'fs clone status '
                   'name=vol_name,type=CephString '
                   'name=clone_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Get status on a cloned subvolume.",
            'perm': 'r'
        },
        {
            'cmd': 'fs clone cancel '
                   'name=vol_name,type=CephString '
                   'name=clone_name,type=CephString '
                   'name=group_name,type=CephString,req=false ',
            'desc': "Cancel an pending or ongoing clone operation.",
            'perm': 'r'
        },
        {
            'cmd': 'nfs export create cephfs '
            'name=fsname,type=CephString '
            'name=clusterid,type=CephString '
            'name=binding,type=CephString '
            'name=readonly,type=CephBool,req=false '
            'name=path,type=CephString,req=false ',
            'desc': "Create a cephfs export",
            'perm': 'rw'
        },
        {
            'cmd': 'nfs export delete '
                   'name=clusterid,type=CephString '
                   'name=binding,type=CephString ',
            'desc': "Delete a cephfs export",
            'perm': 'rw'
        },
        {
            'cmd': 'nfs export ls '
                   'name=clusterid,type=CephString '
                   'name=detailed,type=CephBool,req=false ',
            'desc': "List exports of a NFS cluster",
            'perm': 'r'
        },
        {
            'cmd': 'nfs export get '
                   'name=clusterid,type=CephString '
                   'name=binding,type=CephString ',
            'desc': "Fetch a export of a NFS cluster given the pseudo path/binding",
            'perm': 'r'
        },
        {
            'cmd': 'nfs cluster create '
                   'name=type,type=CephString '
                   'name=clusterid,type=CephString,goodchars=[A-Za-z0-9-_.] '
                   'name=placement,type=CephString,req=false ',
            'desc': "Create an NFS Cluster",
            'perm': 'rw'
        },
        {
            'cmd': 'nfs cluster update '
                   'name=clusterid,type=CephString '
                   'name=placement,type=CephString ',
            'desc': "Updates an NFS Cluster",
            'perm': 'rw'
        },
        {
            'cmd': 'nfs cluster delete '
                   'name=clusterid,type=CephString ',
            'desc': "Deletes an NFS Cluster",
            'perm': 'rw'
        },
        {
            'cmd': 'nfs cluster ls ',
            'desc': "List NFS Clusters",
            'perm': 'r'
        },
        {
            'cmd': 'nfs cluster info '
                   'name=clusterid,type=CephString,req=false ',
            'desc': "Displays NFS Cluster info",
            'perm': 'r'
        },
        {
            'cmd': 'nfs cluster config set '
                   'name=clusterid,type=CephString ',
            'desc': "Set NFS-Ganesha config by `-i <config_file>`",
            'perm': 'rw'
        },
        {
            'cmd': 'nfs cluster config reset '
                   'name=clusterid,type=CephString ',
            'desc': "Reset NFS-Ganesha Config to default",
            'perm': 'rw'
        },
        # volume ls [recursive]
        # subvolume ls <volume>
        # volume authorize/deauthorize
        # subvolume authorize/deauthorize

        # volume describe (free space, etc)
        # volume auth list (vc.get_authorized_ids)

        # snapshots?

        # FIXME: we're doing CephFSVolumeClient.recover on every
        # path where we instantiate and connect a client.  Perhaps
        # keep clients alive longer, or just pass a "don't recover"
        # flag in if it's the >1st time we connected a particular
        # volume in the lifetime of this module instance.
    ]

    MODULE_OPTIONS = [
        {
            'name': 'max_concurrent_clones',
            'type': 'int',
            'default': 4,
            'desc': 'Number of asynchronous cloner threads',
        }
    ]

    def __init__(self, *args, **kwargs):
        self.inited = False
        # for mypy
        self.max_concurrent_clones = None
        self.lock = threading.Lock()
        super(Module, self).__init__(*args, **kwargs)
        # Initialize config option members
        self.config_notify()
        with self.lock:
            self.vc = VolumeClient(self)
            self.fs_export = FSExport(self)
            self.nfs = NFSCluster(self)
            self.inited = True

    def __del__(self):
        self.vc.shutdown()

    def shutdown(self):
        self.vc.shutdown()

    def config_notify(self):
        """
        This method is called whenever one of our config options is changed.
        """
        with self.lock:
            for opt in self.MODULE_OPTIONS:
                setattr(self,
                        opt['name'],  # type: ignore
                        self.get_module_option(opt['name']))  # type: ignore
                self.log.debug(' mgr option %s = %s',
                               opt['name'], getattr(self, opt['name']))  # type: ignore
                if self.inited:
                    if opt['name'] == "max_concurrent_clones":
                        self.vc.cloner.reconfigure_max_concurrent_clones(self.max_concurrent_clones)

    def handle_command(self, inbuf, cmd):
        handler_name = "_cmd_" + cmd['prefix'].replace(" ", "_")
        try:
            handler = getattr(self, handler_name)
        except AttributeError:
            return -errno.EINVAL, "", "Unknown command"

        return handler(inbuf, cmd)

    @mgr_cmd_wrap
    def _cmd_fs_volume_create(self, inbuf, cmd):
        vol_id = cmd['name']
        placement = cmd.get('placement', '')
        return self.vc.create_fs_volume(vol_id, placement)

    @mgr_cmd_wrap
    def _cmd_fs_volume_rm(self, inbuf, cmd):
        vol_name = cmd['vol_name']
        confirm = cmd.get('yes-i-really-mean-it', None)
        return self.vc.delete_fs_volume(vol_name, confirm)

    @mgr_cmd_wrap
    def _cmd_fs_volume_ls(self, inbuf, cmd):
        return self.vc.list_fs_volumes()

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_create(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), empty string(str), error message (str)
        """
        return self.vc.create_subvolume_group(
            vol_name=cmd['vol_name'], group_name=cmd['group_name'],
            pool_layout=cmd.get('pool_layout', None), mode=cmd.get('mode', '755'),
            uid=cmd.get('uid', None), gid=cmd.get('gid', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_rm(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), empty string(str), error message (str)
        """
        return self.vc.remove_subvolume_group(vol_name=cmd['vol_name'],
                                              group_name=cmd['group_name'],
                                              force=cmd.get('force', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_ls(self, inbuf, cmd):
        return self.vc.list_subvolume_groups(vol_name=cmd['vol_name'])

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_create(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), empty string(str), error message (str)
        """
        return self.vc.create_subvolume(vol_name=cmd['vol_name'],
                                        sub_name=cmd['sub_name'],
                                        group_name=cmd.get('group_name', None),
                                        size=cmd.get('size', None),
                                        pool_layout=cmd.get('pool_layout', None),
                                        uid=cmd.get('uid', None),
                                        gid=cmd.get('gid', None),
                                        mode=cmd.get('mode', '755'),
                                        namespace_isolated=cmd.get('namespace_isolated', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_rm(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), empty string(str), error message (str)
        """
        return self.vc.remove_subvolume(vol_name=cmd['vol_name'],
                                        sub_name=cmd['sub_name'],
                                        group_name=cmd.get('group_name', None),
                                        force=cmd.get('force', False),
                                        retain_snapshots=cmd.get('retain_snapshots', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_authorize(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), secret key(str), error message (str)
        """
        return self.vc.authorize_subvolume(vol_name=cmd['vol_name'],
                                           sub_name=cmd['sub_name'],
                                           auth_id=cmd['auth_id'],
                                           group_name=cmd.get('group_name', None),
                                           access_level=cmd.get('access_level', 'rw'),
                                           tenant_id=cmd.get('tenant_id', None),
                                           allow_existing_id=cmd.get('allow_existing_id', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_deauthorize(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), empty string(str), error message (str)
        """
        return self.vc.deauthorize_subvolume(vol_name=cmd['vol_name'],
                                             sub_name=cmd['sub_name'],
                                             auth_id=cmd['auth_id'],
                                             group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_authorized_list(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), list of authids(json), error message (str)
        """
        return self.vc.authorized_list(vol_name=cmd['vol_name'],
                                       sub_name=cmd['sub_name'],
                                       group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_evict(self, inbuf, cmd):
        """
        :return: a 3-tuple of return code(int), empyt string(str), error message (str)
        """
        return self.vc.evict(vol_name=cmd['vol_name'],
                             sub_name=cmd['sub_name'],
                             auth_id=cmd['auth_id'],
                             group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_ls(self, inbuf, cmd):
        return self.vc.list_subvolumes(vol_name=cmd['vol_name'],
                                       group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_getpath(self, inbuf, cmd):
        return self.vc.getpath_subvolume_group(
            vol_name=cmd['vol_name'], group_name=cmd['group_name'])

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_getpath(self, inbuf, cmd):
        return self.vc.subvolume_getpath(vol_name=cmd['vol_name'],
                                         sub_name=cmd['sub_name'],
                                         group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_info(self, inbuf, cmd):
        return self.vc.subvolume_info(vol_name=cmd['vol_name'],
                                      sub_name=cmd['sub_name'],
                                      group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_pin(self, inbuf, cmd):
        return self.vc.pin_subvolume_group(vol_name=cmd['vol_name'],
            group_name=cmd['group_name'], pin_type=cmd['pin_type'],
            pin_setting=cmd['pin_setting'])

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_snapshot_create(self, inbuf, cmd):
        return self.vc.create_subvolume_group_snapshot(vol_name=cmd['vol_name'],
                                                       group_name=cmd['group_name'],
                                                       snap_name=cmd['snap_name'])

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_snapshot_rm(self, inbuf, cmd):
        return self.vc.remove_subvolume_group_snapshot(vol_name=cmd['vol_name'],
                                                       group_name=cmd['group_name'],
                                                       snap_name=cmd['snap_name'],
                                                       force=cmd.get('force', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolumegroup_snapshot_ls(self, inbuf, cmd):
        return self.vc.list_subvolume_group_snapshots(vol_name=cmd['vol_name'],
                                                      group_name=cmd['group_name'])

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_create(self, inbuf, cmd):
        return self.vc.create_subvolume_snapshot(vol_name=cmd['vol_name'],
                                                 sub_name=cmd['sub_name'],
                                                 snap_name=cmd['snap_name'],
                                                 group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_rm(self, inbuf, cmd):
        return self.vc.remove_subvolume_snapshot(vol_name=cmd['vol_name'],
                                                 sub_name=cmd['sub_name'],
                                                 snap_name=cmd['snap_name'],
                                                 group_name=cmd.get('group_name', None),
                                                 force=cmd.get('force', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_info(self, inbuf, cmd):
        return self.vc.subvolume_snapshot_info(vol_name=cmd['vol_name'],
                                               sub_name=cmd['sub_name'],
                                               snap_name=cmd['snap_name'],
                                               group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_ls(self, inbuf, cmd):
        return self.vc.list_subvolume_snapshots(vol_name=cmd['vol_name'],
                                                sub_name=cmd['sub_name'],
                                                group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_resize(self, inbuf, cmd):
        return self.vc.resize_subvolume(vol_name=cmd['vol_name'], sub_name=cmd['sub_name'],
                                        new_size=cmd['new_size'], group_name=cmd.get('group_name', None),
                                        no_shrink=cmd.get('no_shrink', False))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_pin(self, inbuf, cmd):
        return self.vc.subvolume_pin(vol_name=cmd['vol_name'],
            sub_name=cmd['sub_name'], pin_type=cmd['pin_type'],
            pin_setting=cmd['pin_setting'],
            group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_protect(self, inbuf, cmd):
        return self.vc.protect_subvolume_snapshot(vol_name=cmd['vol_name'], sub_name=cmd['sub_name'],
                                                  snap_name=cmd['snap_name'], group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_unprotect(self, inbuf, cmd):
        return self.vc.unprotect_subvolume_snapshot(vol_name=cmd['vol_name'], sub_name=cmd['sub_name'],
                                                    snap_name=cmd['snap_name'], group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_subvolume_snapshot_clone(self, inbuf, cmd):
        return self.vc.clone_subvolume_snapshot(
            vol_name=cmd['vol_name'], sub_name=cmd['sub_name'], snap_name=cmd['snap_name'],
            group_name=cmd.get('group_name', None), pool_layout=cmd.get('pool_layout', None),
            target_sub_name=cmd['target_sub_name'], target_group_name=cmd.get('target_group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_clone_status(self, inbuf, cmd):
        return self.vc.clone_status(
            vol_name=cmd['vol_name'], clone_name=cmd['clone_name'],  group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_fs_clone_cancel(self, inbuf, cmd):
        return self.vc.clone_cancel(
            vol_name=cmd['vol_name'], clone_name=cmd['clone_name'],  group_name=cmd.get('group_name', None))

    @mgr_cmd_wrap
    def _cmd_nfs_export_create_cephfs(self, inbuf, cmd):
        #TODO Extend export creation for rgw.
        return self.fs_export.create_export(fs_name=cmd['fsname'], cluster_id=cmd['clusterid'],
                pseudo_path=cmd['binding'], read_only=cmd.get('readonly', False), path=cmd.get('path', '/'))

    @mgr_cmd_wrap
    def _cmd_nfs_export_delete(self, inbuf, cmd):
        return self.fs_export.delete_export(cluster_id=cmd['clusterid'], pseudo_path=cmd['binding'])

    @mgr_cmd_wrap
    def _cmd_nfs_export_ls(self, inbuf, cmd):
        return self.fs_export.list_exports(cluster_id=cmd['clusterid'], detailed=cmd.get('detailed', False))

    @mgr_cmd_wrap
    def _cmd_nfs_export_get(self, inbuf, cmd):
        return self.fs_export.get_export(cluster_id=cmd['clusterid'], pseudo_path=cmd['binding'])

    @mgr_cmd_wrap
    def _cmd_nfs_cluster_create(self, inbuf, cmd):
        return self.nfs.create_nfs_cluster(cluster_id=cmd['clusterid'], export_type=cmd['type'],
                                           placement=cmd.get('placement', None))

    @mgr_cmd_wrap
    def _cmd_nfs_cluster_update(self, inbuf, cmd):
        return self.nfs.update_nfs_cluster(cluster_id=cmd['clusterid'], placement=cmd['placement'])

    @mgr_cmd_wrap
    def _cmd_nfs_cluster_delete(self, inbuf, cmd):
        return self.nfs.delete_nfs_cluster(cluster_id=cmd['clusterid'])

    @mgr_cmd_wrap
    def _cmd_nfs_cluster_ls(self, inbuf, cmd):
        return self.nfs.list_nfs_cluster()

    @mgr_cmd_wrap
    def _cmd_nfs_cluster_info(self, inbuf, cmd):
        return self.nfs.show_nfs_cluster_info(cluster_id=cmd.get('clusterid', None))

    def _cmd_nfs_cluster_config_set(self, inbuf, cmd):
        return self.nfs.set_nfs_cluster_config(cluster_id=cmd['clusterid'], nfs_config=inbuf)

    def _cmd_nfs_cluster_config_reset(self, inbuf, cmd):
        return self.nfs.reset_nfs_cluster_config(cluster_id=cmd['clusterid'])
