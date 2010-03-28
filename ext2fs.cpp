/**
 * Ext2read
 * File: partition.cpp
 **/
/**
 * Copyright (C) 2005 2010 by Manish Regmi   (regmi dot manish at gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 **/
 /**
  * This file contains implementation of scanning and retrieving
  * partition information. For now we only support MBR style partitions.
  **/

#include "ext2read.h"

Ext2Partition::Ext2Partition(lloff_t size, lloff_t offset, int ssize, FileHandle phandle)
{
    int ret;

    total_sectors = size;
    relative_sect = offset;
    handle = phandle;
    sect_size = ssize;
    onview = false;
    inode_buffer = NULL;
    hint.dind = hint.ind = NULL;
    hint.ind_hint = hint.dind_hint = 0;
    //has_extent = 1;
    ret = mount();
    if(ret < 0)
        return;

    root = read_inode(EXT2_ROOT_INO);
    if(!root)
    {
        LOG("Cannot read the root of %s \n", linux_name.c_str());
        return;
    }

   root->file_name = linux_name;
   root->file_type = 0x02;   //FIXME: do not hardcode
}

Ext2Partition::~Ext2Partition()
{
    free (desc);
}

void Ext2Partition::set_linux_name(const char *name, int disk, int partition)
{
    char dchar = 'a' + disk;
    char pchar = '1' + partition;


    linux_name = name;
    linux_name.append(1, dchar);
    linux_name.append(1, pchar);
}

string &Ext2Partition::get_linux_name()
{
    return linux_name;
}

int Ext2Partition::ext2_readblock(lloff_t blocknum, void *buffer)
{
        int nsects = blocksize/sect_size;
        lloff_t sectno = (lloff_t)((lloff_t)(blocksize/sect_size) * blocknum) + relative_sect;

        return read_disk(handle, buffer, sectno, nsects, sect_size);
}

int Ext2Partition::mount()
{
    EXT2_SUPER_BLOCK sblock;
    int gSizes, gSizeb;		/* Size of total group desc in sectors */
    char *tmpbuf;

    read_disk(handle, &sblock, relative_sect + 2, 2, sect_size);	/* read superBlock of root */
    if(sblock.s_magic != EXT2_SUPER_MAGIC)
    {
            LOG("Bad Super Block. The drive %s is not ext2 formatted.\n", linux_name.c_str());
            return -1;
    }

    blocksize = EXT2_BLOCK_SIZE(&sblock);
    inodes_per_group = EXT2_INODES_PER_GROUP(&sblock);
    inode_size = EXT2_INODE_SIZE(&sblock);

    LOG("Block size %d, inp %d, inodesize %d\n", blocksize, inodes_per_group, inode_size);
    totalGroups = (sblock.s_blocks_count)/EXT2_BLOCKS_PER_GROUP(&sblock);
    gSizeb = (sizeof(EXT2_GROUP_DESC) * totalGroups);
    gSizes = (gSizeb / sect_size)+1;

    desc = (EXT2_GROUP_DESC *) calloc(totalGroups, sizeof(EXT2_GROUP_DESC));
    if(desc == NULL)
    {
        LOG("Not enough Memory: mount: desc: Exiting\n");
            exit(1);
    }

    if((tmpbuf = (char *) malloc(gSizes * sect_size)) == NULL)
    {
        LOG("Not enough Memory: mount: tmpbuf: Exiting\n");
            exit(1);
    }

    /* Read all Group descriptors and store in buffer */
    /* I really dont know the official start location of Group Descriptor array */
    if((blocksize/sect_size) <= 2)
            read_disk(handle, tmpbuf, relative_sect + ((blocksize/sect_size) + 2), gSizes, sect_size);
    else
            read_disk(handle, tmpbuf, relative_sect + (blocksize/sect_size), gSizes, sect_size);

    memcpy(desc, tmpbuf, gSizeb);

    free(tmpbuf);

    return 0;
}

EXT2DIRENT *Ext2Partition::open_dir(Ext2File *parent)
{
    EXT2DIRENT *dirent;

    if(!parent)
        return NULL;

    dirent = new EXT2DIRENT;
    dirent->parent = parent;
    dirent->next = NULL;
    dirent->dirbuf = NULL;

    return dirent;
}

Ext2File *Ext2Partition::read_dir(EXT2DIRENT *dirent)
{
    Ext2File *newEntry;
    char *pos;

    if(!dirent)
        return NULL;
    if(!dirent->dirbuf)
    {
        dirent->dirbuf = (EXT2_DIR_ENTRY *) new char[blocksize];
        if(!dirent->dirbuf)
            return NULL;
        read_data_block(&dirent->parent->inode, 0, dirent->dirbuf);
    }

    if(!dirent->next)
        dirent->next = dirent->dirbuf;
    else
    {
        pos = (char *) dirent->next;
        dirent->next = (EXT2_DIR_ENTRY *)(pos + dirent->next->rec_len);
        if(IS_BUFFER_END(dirent->next, dirent->dirbuf, blocksize))
        {
            dirent->next = NULL;
            return NULL;
        }
    }

    newEntry = read_inode(dirent->next->inode);
    if(!newEntry)
    {
        LOG("Error reading Inode %d parent inode %d.\n", dirent->next->inode, dirent->parent->inode_num);
        return NULL;
    }

    newEntry->file_type = dirent->next->filetype;
    newEntry->file_name.assign(dirent->next->name, dirent->next->name_len);

    return newEntry;
}

void Ext2Partition::close_dir(EXT2DIRENT *dirent)
{
    delete [] dirent->dirbuf;
    delete dirent;
}

Ext2File *Ext2Partition::read_inode(uint32_t inum)
{
    uint32_t group, index, blknum;
    int inode_index, ret = 0;
    Ext2File *file = NULL;
    EXT2_INODE *src;

    if(inum == 0)
        return NULL;

    if(!inode_buffer)
    {
        inode_buffer = (char *)malloc(blocksize);
        if(!inode_buffer)
            return NULL;
    }

    group = (inum - 1) / inodes_per_group;

    if(group > totalGroups)
    {
        LOG("Error Reading Inode %X. Invalid Inode Number\n", inum);
        return NULL;
    }

    index = ((inum - 1) % inodes_per_group) * inode_size;
    inode_index = (index % blocksize);
    blknum = desc[group].bg_inode_table + (index / blocksize);


    if(blknum != last_block)
        ret = ext2_readblock(blknum, inode_buffer);

    file = new Ext2File;
    if(!file)
    {
        LOG("Allocation of File Failed. \n");
        return NULL;
    }
    src = (EXT2_INODE *)(inode_buffer + inode_index);
    file->inode = *src;

    //LOG("BLKNUM is %d, inode_index %d\n", file->inode.i_size, inode_index);
    file->inode_num = inum;
    file->partition = (Ext2Partition *)this;
    file->onview = false;

    last_block = blknum;

    return file;
}


int Ext2Partition::read_data_block(EXT2_INODE *ino, lloff_t lbn, void *buf)
{
    lloff_t block;

        if(INODE_HAS_EXTENT(ino))
            block = extent_to_logical(ino, lbn);
        else
            block = fileblock_to_logical(ino, lbn);

        if(block == 0)
            return -1;

        return ext2_readblock(block, buf);
}

lloff_t Ext2Partition::extent_to_logical(EXT2_INODE *ino, lloff_t lbn)
{
    struct ext4_extent_header *header;
    struct ext4_extent *extent;

    lloff_t block = 0;

    header  = get_ext4_header(ino);
    if(header->eh_magic != EXT4_EXT_MAGIC)
    {
        LOG("Invalid magic in Extent Header: %X\n", header->eh_magic);
        return 0;
    }
    extent = EXT_FIRST_EXTENT(header);
    if(header->eh_depth == 0)
    {
        for(int i = 0; i < header->eh_entries; i++)
        {
            extent = extent + i;
            if((lbn >= extent->ee_block) &&
               (lbn < (extent->ee_block + extent->ee_len)))
            {
                block = block - (lloff_t)extent->ee_block;
                block += ext_to_block(extent);
                break;
            }
        }
    }
    return block;
}

uint32_t Ext2Partition::fileblock_to_logical(EXT2_INODE *ino, uint32_t lbn)
{
    uint32_t block, indlast, dindlast;
    uint32_t tmpblk, sz;

    if(lbn < EXT2_NDIR_BLOCKS)
    {
            return ino->i_block[lbn];
    }

    sz = blocksize / sizeof(uint32_t);
    indlast = sz + EXT2_NDIR_BLOCKS;

    if((lbn >= EXT2_NDIR_BLOCKS) && (lbn < indlast))
    {
            block = ino->i_block[EXT2_IND_BLOCK];
            if(hint.ind_hint != block)
            {
                if(!hint.ind)
                {
                    hint.ind = (uint32_t *) new char [blocksize];
                    if(!hint.ind)
                        return 0;
                }
                ext2_readblock(block, hint.ind);
                hint.ind_hint = block;
            }

            lbn -= EXT2_NDIR_BLOCKS;
            return hint.ind[lbn];
    }

    dindlast = (sz * sz) + indlast;
    if((lbn >= indlast) && (lbn < dindlast))
    {
            block = ino->i_block[EXT2_DIND_BLOCK];
            if(hint.dind_hint != block)
            {
                if(!hint.dind)
                {
                    hint.dind = (uint32_t *) new char [blocksize];
                    if(!hint.ind)
                        return 0;
                }
                ext2_readblock(block, hint.dind);
                hint.dind_hint = block;
            }

            tmpblk = lbn - indlast;
            block = hint.dind[tmpblk/sz];
            if(block != hint.ind_hint)
            {
                hint.ind_hint = block;
                ext2_readblock(block, hint.ind);
            }
            lbn = tmpblk % sz;
            return hint.ind[lbn];
    }

    ///tindlast = (sz * sz * sz) + dindlast;
    if(lbn >= dindlast)
    {
            block = ino->i_block[EXT2_TIND_BLOCK];
            if(block != hint.tind_hint)
            {
                if(!hint.tind)
                {
                    hint.tind = (uint32_t *) new char [blocksize];
                    if(!hint.tind)
                        return 0;
                }
                hint.tind_hint = block;
                ext2_readblock(block, hint.tind);
            }

            tmpblk = lbn - dindlast;
            block = hint.tind[tmpblk/(sz * sz)];
            if(block != hint.dind_hint)
            {
                    hint.dind_hint = block;
                    ext2_readblock(block, hint.dind);
            }
            block = tmpblk / sz;
            lbn = tmpblk % sz;
            block = hint.dind[block];
            if(block != hint.ind_hint)
            {
                    hint.ind_hint = block;
                    ext2_readblock(block, hint.ind);
            }
            return hint.ind[lbn];
    }

    return 0;
}
