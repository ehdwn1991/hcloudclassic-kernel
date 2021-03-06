#ifndef NO_SHM

#include <linux/ipc.h>
#include <linux/ipc_namespace.h>
#include <linux/shm.h>
#include <linux/msg.h>
#include <gdm/gdm.h>
#include <hcloud/hotplug.h>
#include "hccshm.h"
#include "ipc_handler.h"
#include "shm_handler.h"
#include "shmid_io_linker.h"
#include "ipcmap_io_linker.h"
#include "shm_memory_linker.h"

static struct kern_ipc_perm *hcb_ipc_shm_lock(struct ipc_ids *ids, int id)
{
	shmid_object_t *shp_object;
	struct shmid_kernel *shp;
	int index;

	rcu_read_lock();

	index = ipcid_to_idx(id);

	shp_object = _gdm_grab_object_no_ft(ids->hccops->data_gdm_set, index);

	if (!shp_object)
		goto error;

	shp = shp_object->local_shp;

	BUG_ON(!shp);

	mutex_lock(&shp->shm_perm.mutex);

	if (shp->shm_perm.deleted) {
		mutex_unlock(&shp->shm_perm.mutex);
		goto error;
	}

	return &(shp->shm_perm);

error:
	_gdm_put_object(ids->hccops->data_gdm_set, index);
	rcu_read_unlock();

	return ERR_PTR(-EINVAL);
}



static void hcb_ipc_shm_unlock(struct kern_ipc_perm *ipcp)
{
	int index, deleted = 0;

	index = ipcid_to_idx(ipcp->id);

	if (ipcp->deleted)
		deleted = 1;

	_gdm_put_object(ipcp->hccops->data_gdm_set, index);

	if (!deleted)
		mutex_unlock(&ipcp->mutex);

	rcu_read_unlock();
}

static struct kern_ipc_perm *hcb_ipc_shm_findkey(struct ipc_ids *ids, key_t key)
{
	long *key_index;
	int id = -1;

	key_index = _gdm_get_object_no_ft(ids->hccops->key_gdm_set, key);

	if (key_index)
		id = *key_index;

	_gdm_put_object(ids->hccops->key_gdm_set, key);

	if (id != -1)
		return hcb_ipc_shm_lock(ids, id);

	return NULL;
}


int hcc_ipc_shm_newseg (struct ipc_namespace *ns, struct shmid_kernel *shp)
{
	shmid_object_t *shp_object;
	struct gdm_set *gdm;
	long *key_index;
	int index, err;

	BUG_ON(!shm_ids(ns).hccops);

	index = ipcid_to_idx(shp->shm_perm.id);

	shp_object = gdm_grab_object_manual_ft(
		shm_ids(ns).hccops->data_set, index);

	BUG_ON(shp_object);

	shp_object = kmem_cache_alloc(shmid_object_cachep, GFP_KERNEL);
	if (!shp_object) {
		err = -ENOMEM;
		goto err_put;
	}

	gdm = gdm_create_new_set (gdm_def_ns, 0, SHM_MEMORY_LINKER,
				     hcloud_node_id, PAGE_SIZE,
				     &shp->shm_perm.id, sizeof(int), 0);

	if (IS_ERR(gdm)) {
		err = PTR_ERR(gdm);
		goto err_put;
	}

	shp->shm_file->f_dentry->d_inode->i_mapping->gdm_set = gdm;
	shp->shm_file->f_op = &hcc_shm_file_operations;

	shp_object->set_id = gdm->id;

	shp_object->local_shp = shp;

	_set_object(shm_ids(ns).hccops->data_set, index, shp_object);

	if (shp->shm_perm.key != IPC_PRIVATE)
	{
		key_index = _grab_object(shm_ids(ns).hccops->key_set,
					      shp->shm_perm.key);
		*key_index = index;
		_put_object (shm_ids(ns).hccops->key_set,
				  shp->shm_perm.key);
	}

	shp->shm_perm.hccops = shm_ids(ns).hccops;

err_put:
	_put_object(shm_ids(ns).hccops->data_set, index);

	return 0;

}

void hcc_ipc_shm_rmkey(struct ipc_namespace *ns, key_t key)
{
	_remove_object(shm_ids(ns).hccops->key_set, key);
}


void hcc_ipc_shm_destroy(struct ipc_namespace *ns, struct shmid_kernel *shp)
{
	struct hcc_set *mm_set;
	int index;
	key_t key;

	index = ipcid_to_idx(shp->shm_perm.id);
	key = shp->shm_perm.key;

	mm_set = shp->shm_file->f_dentry->d_inode->i_mapping->hcc_set;

	if (key != IPC_PRIVATE) {
		_hcc_grab_object_no_ft(shm_ids(ns).hccops->key_hcc_set, key);
		_hcc_remove_frozen_object(shm_ids(ns).hccops->key_hcc_set, key);
	}

	local_shm_unlock(shp);

	_hcc_remove_frozen_object(shm_ids(ns).hccops->data_hcc_set, index);
	_destroy_hcc_set(mm_set);

	hcc_ipc_rmid(&shm_ids(ns), index);
}


int hcc_shm_init_ns(struct ipc_namespace *ns)
{
	int r;

	struct hccipc_ops *shm_ops = kmalloc(sizeof(struct hccipc_ops),
					     GFP_KERNEL);
	if (!shm_ops) {
		r = -ENOMEM;
		goto err;
	}

	shm_ops->map_gdm_set = create_new_gdm_set(gdm_def_ns,
						    SHMMAP_GDMID,
						    IPCMAP_LINKER,
						    GDMRR_DEF_OWNER,
						    sizeof(ipcmap_object_t),
						    GDMLOCAL_EXCLUSIVE);
	if (IS_ERR(shm_ops->map_gdm_set)) {
		r = PTR_ERR(shm_ops->map_gdm_set);
		goto err_map;
	}

	shm_ops->key_gdm_set = create_new_gdm_set(gdm_def_ns,
						    SHMKEY_GDMID,
						    SHMKEY_LINKER,
						    GDMRR_DEF_OWNER,
						    sizeof(long),
						    GDMLOCAL_EXCLUSIVE);
	if (IS_ERR(shm_ops->key_gdm_set)) {
		r = PTR_ERR(shm_ops->key_gdm_set);
		goto err_key;
	}

	shm_ops->data_gdm_set = create_new_gdm_set(gdm_def_ns,
						     SHMID_GDMID,
						     SHMID_LINKER,
						     GDMRR_DEF_OWNER,
						     sizeof(shmid_object_t),
						     GDMLOCAL_EXCLUSIVE);
	if (IS_ERR(shm_ops->data_gdm_set)) {
		r = PTR_ERR(shm_ops->data_gdm_set);
		goto err_data;
	}

	shm_ops->ipc_lock = kcb_ipc_shm_lock;
	shm_ops->ipc_unlock = kcb_ipc_shm_unlock;
	shm_ops->ipc_findkey = kcb_ipc_shm_findkey;

	shm_ids(ns).hccops = shm_ops;

	return 0;


err_data:
	_destroy_gdm_set(shm_ops->key_gdm_set);
err_key:
	_destroy_gdm_set(shm_ops->map_gdm_set);
err_map:
	kfree(shm_ops);
err:
	return r;
}



void hcc_shm_exit_ns(struct ipc_namespace *ns)
{
	if (shm_ids(ns).hccops) {

		_destroy_gdm_set(shm_ids(ns).hccops->data_gdm_set);
		_destroy_gdm_set(shm_ids(ns).hccops->key_gdm_set);
		_destroy_gdm_set(shm_ids(ns).hccops->map_gdm_set);

		kfree(shm_ids(ns).hccops);
	}
}
void shm_handler_init(void)
{
	shmid_object_cachep = kmem_cache_create("shmid_object",
						sizeof(shmid_object_t),
						0, SLAB_PANIC, NULL);

	register_io_linker(SHM_MEMORY_LINKER, &shm_memory_linker);
	register_io_linker(SHMID_LINKER, &shmid_linker);
	register_io_linker(SHMKEY_LINKER, &shmkey_linker);

	hccsyms_register(HCCSYMS_VM_OPS_SHM, &shm_vm_ops);

	printk("Shm Server configured\n");
}

void shm_handler_finalize (void)
{
}
#endif