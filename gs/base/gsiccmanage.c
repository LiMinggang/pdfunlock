/* Copyright (C) 2001-2009 Artifex Software, Inc.
   All Rights Reserved.
  
   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied, modified
   or distributed except as expressly authorized under the terms of that
   license.  Refer to licensing information at http://www.artifex.com/
   or contact Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134,
   San Rafael, CA  94903, U.S.A., +1(415)492-9861, for further information.
*/
/*  GS ICC Manager.  Initial stubbing of functions.  */

#include "std.h"
#include "stdpre.h"
#include "gstypes.h"
#include "gsmemory.h"
#include "gsstruct.h"  
#include "scommon.h"
#include "strmio.h"
#include "gx.h"
#include "gxistate.h"
#include "gxcspace.h"
#include "gscms.h"
#include "gsiccmanage.h"
#include "gsicccache.h"
#include "gsicc_profilecache.h"
#include "gserrors.h"
#include "string_.h"

#if ICC_DUMP
unsigned int global_icc_index = 0;
#endif

/* profile data structure */
/* profile_handle should NOT be garbage collected since it is allocated by the external CMS */

gs_private_st_ptrs2(st_gsicc_profile, cmm_profile_t, "gsicc_profile",
		    gsicc_profile_enum_ptrs, gsicc_profile_reloc_ptrs, buffer, name);

gs_private_st_ptrs9(st_gsicc_manager, gsicc_manager_t, "gsicc_manager",
		    gsicc_manager_enum_ptrs, gsicc_manager_profile_reloc_ptrs,
		    device_profile, device_named, default_gray, default_rgb,
                    default_cmyk, proof_profile, output_link, lab_profile, profiledir); 

static const gs_color_space_type gs_color_space_type_icc = {
    gs_color_space_index_ICC,       /* index */
    true,                           /* can_be_base_space */
    true,                           /* can_be_alt_space */
    NULL                            /* This is going to be outside the norm. struct union*/

}; 



/* Get the size of the ICC profile that is in the buffer */
unsigned int gsicc_getprofilesize(unsigned char *buffer)
{

    return ( (buffer[0] << 24) + (buffer[1] << 16) +
             (buffer[2] << 8)  +  buffer[3] );

}



/*  This sets the directory to prepend to the ICC profile names specified for
    defaultgray, defaultrgb, defaultcmyk, proofing, linking, named color and device */

void
gsicc_set_icc_directory(const gs_imager_state *pis, const char* pname, int namelen)
{

    gsicc_manager_t *icc_manager = pis->icc_manager;
    char *result;
    gs_memory_t *mem_gc = pis->memory; 


    result = gs_alloc_bytes(mem_gc, namelen,
		   		     "gsicc_set_icc_directory");

    if (result != NULL) {

        strcpy(result, pname);

        icc_manager->profiledir = result;
        icc_manager->namelen = namelen;

    }

}


/*  This computes the hash code for the
    ICC data and assigns the code
    and the profile to the appropriate
    member variable in the ICC manager */

int 
gsicc_set_profile(const gs_imager_state * pis, const char* pname, int namelen, gsicc_profile_t defaulttype)
{

    gsicc_manager_t *icc_manager = pis->icc_manager;
    cmm_profile_t *icc_profile;
    cmm_profile_t **manager_default_profile;
    stream *str;
    gs_memory_t *mem_gc = pis->memory; 
    int code;
    int k;
        
       /* For now only let this be set once. 
       We could have this changed dynamically
       in which case we need to do some 
       deaallocations prior to replacing 
       it */

    switch(defaulttype){

        case DEFAULT_GRAY:

            manager_default_profile = &(icc_manager->default_gray);

            break;

        case DEFAULT_RGB:

            manager_default_profile = &(icc_manager->default_rgb);

            break;

        case DEFAULT_CMYK:
            
             manager_default_profile = &(icc_manager->default_cmyk);

             break;

        case PROOF_TYPE:

             manager_default_profile = &(icc_manager->proof_profile);

             break;

        case NAMED_TYPE:

             manager_default_profile = &(icc_manager->device_named);

             break;

        case LINKED_TYPE:

             manager_default_profile = &(icc_manager->output_link);

             break;

        case LAB_TYPE:

             manager_default_profile = &(icc_manager->lab_profile);


    }

    /* If it is not NULL then it has already been set.
       If it is different than what we already have then
       we will want to free it.  Since other imager states
       could have different default profiles, this is done via
       reference counting.  If it is the same as what we
       already have then we DONT increment, since that is
       done when the imager state is duplicated.  It could
       be the same, due to a resetting of the user params.
       To avoid recreating the profile data, we compare the 
       string names. */

    if ((*manager_default_profile) != NULL){

        /* Check if this is what we already have */

        icc_profile = *manager_default_profile;

        if ( namelen == icc_profile->name_length )
            if( memcmp(pname, icc_profile->name, namelen) == 0) return 0;

        rc_decrement(icc_profile,"gsicc_profile");

    }

    str = gsicc_open_search(pname, namelen, mem_gc, icc_manager);

    if (str != NULL){

        icc_profile = gsicc_profile_new(str, mem_gc, pname, namelen);
        code = sfclose(str);
        *manager_default_profile = icc_profile;

        /* Get the profile handle */

        icc_profile->profile_handle = gsicc_get_profile_handle_buffer(icc_profile->buffer);

        /* Compute the hash code of the profile. Everything in the ICC manager will have 
           it's hash code precomputed */

        gsicc_get_icc_buff_hash(icc_profile->buffer, &(icc_profile->hashcode));
        icc_profile->hash_is_valid = true;

        icc_profile->default_match = defaulttype;

        icc_profile->num_comps = gscms_get_channel_count(icc_profile->profile_handle);
        icc_profile->data_cs = gscms_get_profile_data_space(icc_profile->profile_handle);

        /* Initialize the range to default values */

        for ( k = 0; k < icc_profile->num_comps; k++) {

            icc_profile->Range.ranges[k].rmin = 0.0;
            icc_profile->Range.ranges[k].rmax = 1.0;

        }

        if (defaulttype == LAB_TYPE) icc_profile->islab = true;
         
        return(0);
       
    }
     
    return(-1);

    
}


/* This is used to try to find the specified or default ICC profiles */

static stream* 
gsicc_open_search(const char* pname, int namelen, gs_memory_t *mem_gc, gsicc_manager_t *icc_manager)
{
    char *buffer;
    stream* str;

    /* Check if we need to prepend the file name  */

    if ( icc_manager->profiledir != NULL ){
        
        /* If this fails, we will still try the file by itself and with %rom%
           since someone may have left a space some of the spaces as our defaults,
           even if they defined the directory to use. This will occur only after
           searching the defined directory.  A warning is noted.  */

        buffer = gs_alloc_bytes(mem_gc, namelen + icc_manager->namelen,
		   		     "gsicc_open_search");

        strcpy(buffer, icc_manager->profiledir);
        strcat(buffer, pname);

        str = sfopen(buffer, "rb", mem_gc);

        gs_free_object(mem_gc, buffer, "gsicc_open_search");

        if (str != NULL)
            return(str);
        else
            gs_warn2("Could not find %s in %s checking default paths",pname,icc_manager->profiledir);

    } 

    /* First just try it like it is */

    str = sfopen(pname, "rb", mem_gc);

    if (str != NULL) return(str);

    /* If that fails, try %rom% */

    buffer = gs_alloc_bytes(mem_gc, namelen + 5,
   		         "gsicc_open_search");

    strcpy(buffer, "%rom%");
    strcat(buffer, pname);

    str = sfopen(buffer, "rb", mem_gc);

    gs_free_object(mem_gc, buffer, "gsicc_open_search");

    if (str == NULL) {

        gs_warn1("Could not find %s in root directory",pname);
        gs_warn1("Could not find %s",buffer);

    }

    return(str);

}

/* This set the device profile entry of the ICC manager.  If the
   device does not have a defined profile, then a default one
   is selected. */

int
gsicc_init_device_profile(gs_state * pgs, gx_device * dev)
{

    const gs_imager_state * pis = (gs_imager_state*) pgs;
    int code;

    /* See if the device has a profile */

    if (dev->color_info.icc_profile[0] == '\0'){

        /* Grab a default one.  Need to think a bit about duo devices.
           We  should handle those as separation devices that have
           empty CMYK planes, but I need to double check that. */

        switch(dev->color_info.num_components){

            case 1:

                strcpy(dev->color_info.icc_profile, DEFAULT_GRAY_ICC);
                break;

            case 3:

                strcpy(dev->color_info.icc_profile, DEFAULT_RGB_ICC);
                break;

            case 4:

                strcpy(dev->color_info.icc_profile, DEFAULT_CMYK_ICC);
                break;

            default:

                strcpy(dev->color_info.icc_profile, DEFAULT_CMYK_ICC);
                break;


        }

    } 

    /* Set the manager */

    if (pis->icc_manager->device_profile == NULL) {

        code = gsicc_set_device_profile(pis->icc_manager, dev, pis->memory);
        return(code);

    }

    return(0);

}


/*  This computes the hash code for the
    device profile and assigns the code
    and the profile to the DeviceProfile
    member variable in the ICC Manager
    This should really occur only one time.
    This is different than gs_set_device_profile
    which sets the profile on the output
    device */

static int 
gsicc_set_device_profile(gsicc_manager_t *icc_manager, gx_device * pdev, gs_memory_t * mem)
{
    cmm_profile_t *icc_profile;
    stream *str;
    const char *profile = &(pdev->color_info.icc_profile[0]);
    int code;

    if (icc_manager->device_profile == NULL){

        /* Device profile in icc manager has not 
           yet been set. Lets do it. */

        /* Check if device has a profile.  This 
           should always be true, since if one was
           not set, we should have set it to the default. 
        */

        if (profile != '\0'){

            str = gsicc_open_search(profile, strlen(profile), mem, icc_manager);

            if (str != NULL){
                icc_profile = gsicc_profile_new(str, mem, profile, strlen(profile));
                code = sfclose(str);
                icc_manager->device_profile = icc_profile;

                /* Get the profile handle */

                icc_profile->profile_handle = gsicc_get_profile_handle_buffer(icc_profile->buffer);

                /* Compute the hash code of the profile. Everything in the ICC manager will have 
                   it's hash code precomputed */

                gsicc_get_icc_buff_hash(icc_profile->buffer, &(icc_profile->hashcode));
                icc_profile->hash_is_valid = true;

                /* Get the number of channels in the output profile */

                icc_profile->num_comps = gscms_get_channel_count(icc_profile->profile_handle);
                icc_profile->data_cs = gscms_get_profile_data_space(icc_profile->profile_handle);

            } else {

                return gs_rethrow(-1, "cannot find device profile");

            }
  

        }


    }

    return(0);
    
}

/* Set the icc profile in the gs_color_space object */

int
gsicc_set_gscs_profile(gs_color_space *pcs, cmm_profile_t *icc_profile, gs_memory_t * mem)
{

    if (pcs != NULL){

#if ICC_DUMP

        dump_icc_buffer(icc_profile->buffer_size, "set_gscs", icc_profile->buffer);
        global_icc_index++;

#endif

        if (pcs->cmm_icc_profile_data != NULL) {

            /* There is already a profile set there */
            /* free it and then set to the new one.  */
            /* should we check the hash code and retain if the same
               or place this job on the caller?  */

            rc_free_icc_profile(mem, (void*) pcs->cmm_icc_profile_data, "gsicc_set_gscs_profile");
            pcs->cmm_icc_profile_data = icc_profile;

            return(0);

        } else {

            pcs->cmm_icc_profile_data = icc_profile;
            return(0);

        }
    }

    return(-1);

}



cmm_profile_t *
gsicc_profile_new(stream *s, gs_memory_t *memory, const char* pname, int namelen)
{


    cmm_profile_t *result;
    int code;
    char *nameptr;

    result = gs_alloc_struct(memory, cmm_profile_t, &st_gsicc_profile,
			     "gsicc_profile_new");

    if (result == NULL) return result; 

    if (namelen > 0){

        nameptr = gs_alloc_bytes(memory, namelen,
	   		     "gsicc_profile_new");
        memcpy(nameptr, pname, namelen);
        result->name = nameptr;

    } else {
    
        result->name = NULL;

    }

    result->name_length = namelen;

    /* We may not have a stream if we are creating this
       object from our own constructed buffer.  For 
       example if we are converting CalRGB to an ICC type */

    if ( s != NULL) {

        code = gsicc_load_profile_buffer(result, s, memory);
        if (code < 0) {

            gs_free_object(memory, result, "gsicc_profile_new");
            return NULL;

        }
    } else {

        result->buffer = NULL;
        result->buffer_size = 0;

    }

    rc_init_free(result, memory, 1, rc_free_icc_profile);

    result->profile_handle = NULL;
    result->hash_is_valid = false;
    result->islab = false;
    result->default_match = DEFAULT_NONE;

    return(result);

}



static void
rc_free_icc_profile(gs_memory_t * mem, void *ptr_in, client_name_t cname)
{
    cmm_profile_t *profile = (cmm_profile_t *)ptr_in;

    if (profile->rc.ref_count <= 1 ){

        /* Clear out the buffer if it is full */

        if(profile->buffer != NULL){

            gs_free_object(mem, profile->buffer, cname);
            profile->buffer = NULL;

        }

        /* Release this handle if it has been set */

        if(profile->profile_handle != NULL){

            gscms_release_profile(profile->profile_handle);
            profile->profile_handle = NULL;

        }

        /* Release the name if it has been set */

        if(profile->name != NULL){

            gs_free_object(mem,profile->name,cname);
            profile->name = NULL;
            profile->name_length = 0;

        }

        profile->hash_is_valid = 0;

	gs_free_object(mem, profile, cname);

    }
}



gsicc_manager_t *
gsicc_manager_new(gs_memory_t *memory)
{

    gsicc_manager_t *result;

    /* Allocated in gc memory */
      
    rc_alloc_struct_1(result, gsicc_manager_t, &st_gsicc_manager, memory, 
                        return(NULL),"gsicc_manager_new");

   result->default_cmyk = NULL;
   result->default_gray = NULL;
   result->default_rgb = NULL;
   result->device_named = NULL;
   result->output_link = NULL;
   result->device_profile = NULL;
   result->proof_profile = NULL;
   result->lab_profile = NULL;
   result->memory = memory;

   result->profiledir = NULL;
   result->namelen = 0;

   return(result);

}

/* Allocates and loads the icc buffer from the stream. */
static int
gsicc_load_profile_buffer(cmm_profile_t *profile, stream *s, gs_memory_t *memory)
{
       
    int                     num_bytes,profile_size;
    unsigned char           buffer_size[4];
    unsigned char           *buffer_ptr;

    srewind(s);  /* Work around for issue with sfread return 0 bytes
                    and not doing a retry if there is an issue.  This
                    is a bug in the stream logic or strmio layer.  Occurs
                    with smask_withicc.pdf on linux 64 bit system */

    buffer_ptr = &(buffer_size[0]);
    num_bytes = sfread(buffer_ptr,sizeof(unsigned char),4,s);
    profile_size = gsicc_getprofilesize(buffer_ptr);

    /* Allocate the buffer, stuff with the profile */

   buffer_ptr = gs_alloc_bytes(memory, profile_size,
					"gsicc_load_profile");

   if (buffer_ptr == NULL){
        return(-1);
   }

   srewind(s);
   num_bytes = sfread(buffer_ptr,sizeof(unsigned char),profile_size,s);

   if( num_bytes != profile_size){
    
       gs_free_object(memory, buffer_ptr, "gsicc_load_profile");
       return(-1);

   }

   profile->buffer = buffer_ptr;
   profile->buffer_size = num_bytes;

   return(0);

}

/* Check if the profile is the same as any of the default profiles */

static void
gsicc_set_default_cs_value(cmm_profile_t *picc_profile, gs_imager_state *pis){

    if ( picc_profile->default_match == DEFAULT_NONE ){

        switch ( picc_profile->data_cs ) {

            case gsGRAY:

                if ( picc_profile->hashcode == pis->icc_manager->default_gray->hashcode )
                    picc_profile->default_match = DEFAULT_GRAY;

                break;

            case gsRGB:

                if ( picc_profile->hashcode == pis->icc_manager->default_gray->hashcode )
                    picc_profile->default_match = DEFAULT_RGB;

                break;

            case gsCMYK:

                if ( picc_profile->hashcode == pis->icc_manager->default_gray->hashcode )
                    picc_profile->default_match = DEFAULT_CMYK;

                break;

        }

    }

}

/* Initialize the hash code value */

void
gsicc_init_hash_cs(cmm_profile_t *picc_profile, gs_imager_state *pis){


    if ( !(picc_profile->hash_is_valid) ) {

        gsicc_get_icc_buff_hash(picc_profile->buffer, &(picc_profile->hashcode));
        picc_profile->hash_is_valid = true;

    }

    gsicc_set_default_cs_value(picc_profile, pis);

}



gcmmhprofile_t
gsicc_get_profile_handle_buffer(unsigned char *buffer){

    gcmmhprofile_t profile_handle = NULL;
    unsigned int profile_size;

     if( buffer != NULL){
         profile_size = gsicc_getprofilesize(buffer);
         profile_handle = gscms_get_profile_handle_mem(buffer, profile_size);
         return(profile_handle);
     }

     return(0);

}
                                

 /*  If we have a profile for the color space already, then we use that.  
     If we do not have one then we will use data from 
     the ICC manager that is based upon the current color space. */

 cmm_profile_t*
 gsicc_get_gscs_profile(gs_color_space *gs_colorspace, gsicc_manager_t *icc_manager)
 {

     cmm_profile_t *profile = gs_colorspace->cmm_icc_profile_data;
     gs_color_space_index color_space_index = gs_color_space_get_index(gs_colorspace);

     if (profile != NULL ) {

        return(profile);

     }

     /* Else, return the default types */

     switch( color_space_index ){

	case gs_color_space_index_DeviceGray:

            return(icc_manager->default_gray);

            break;

	case gs_color_space_index_DeviceRGB:

            return(icc_manager->default_rgb);

            break;

	case gs_color_space_index_DeviceCMYK:

            return(icc_manager->default_cmyk);

            break;

	case gs_color_space_index_DevicePixel:

            /* Not sure yet what our response to 
               this should be */

            return(0);

            break;

       case gs_color_space_index_DeviceN:

            /* If we made it to here, then we will need to use the 
               alternate colorspace */

            return(0);

            break;

       case gs_color_space_index_CIEDEFG:

           /* Need to convert to an ICC form */

            break;

        case gs_color_space_index_CIEDEF:

           /* Need to convert to an ICC form */

            break;

        case gs_color_space_index_CIEABC:

           /* Need to convert to an ICC form */

            break;

        case gs_color_space_index_CIEA:

           /* Need to convert to an ICC form */

            break;

        case gs_color_space_index_Separation:

            /* Caller should use named color path */

            return(0);

            break;

        case gs_color_space_index_Pattern:
        case gs_color_space_index_Indexed:

            /* Caller should use the base space for these */
            return(0);

            break;


        case gs_color_space_index_ICC:

            /* This should not occur, as the space
               should have had a populated profile handle */

            return(0);

            break;

     } 


    return(0);


 }


static cmm_profile_t*
gsicc_get_profile( gsicc_profile_t profile_type, gsicc_manager_t *icc_manager ) {

    switch (profile_type) {

        case DEFAULT_GRAY:

            return(icc_manager->default_gray);

        case DEFAULT_RGB:

            return(icc_manager->default_rgb);

        case DEFAULT_CMYK:
            
             return(icc_manager->default_cmyk);

        case PROOF_TYPE:

             return(icc_manager->proof_profile);

        case NAMED_TYPE:

             return(icc_manager->device_named);

        case LINKED_TYPE:

             return(icc_manager->output_link);

        case LAB_TYPE:

             return(icc_manager->lab_profile);

    }

    return(NULL);

 }

void 
gsicc_profile_serialize(gsicc_serialized_profile_t *profile_data, cmm_profile_t *icc_profile)
{
    
    int k;
  
    if (icc_profile == NULL)
        return;

    profile_data->buffer_size = icc_profile->buffer_size;
    profile_data->data_cs = icc_profile->data_cs;
    profile_data->default_match = icc_profile->default_match;
    profile_data->hash_is_valid = icc_profile->hash_is_valid;
    profile_data->hashcode = icc_profile->hashcode;
    profile_data->islab = icc_profile->islab;
    profile_data->num_comps = icc_profile->num_comps;
   
    for ( k = 0; k < profile_data->num_comps; k++ ){

        profile_data->Range.ranges[k].rmax = 
            icc_profile->Range.ranges[k].rmax;
   
        profile_data->Range.ranges[k].rmin = 
            icc_profile->Range.ranges[k].rmin;

    }


}

int
gsicc_profile_clist_read(
    cmm_profile_t         *icc_profile,     
    const gs_imager_state * pis,
    uint		    offset,
    const byte *            data,
    uint                    size,
    gs_memory_t *           mem )
{
    const byte *dp = data;
    int left = size;
    int profile_size, k;
    gsicc_serialized_profile_t profile_data;

    if (size == sizeof(icc_profile->default_match) && offset == 0 ) {

            /* Profile is one of the default types */

        gsicc_profile_t profile_type; 

        memcpy(&profile_type, dp, sizeof(profile_type));

        icc_profile = gsicc_get_profile( profile_type, pis->icc_manager );
        rc_increment( icc_profile );

        return size;
    }

    if (size == sizeof( icc_profile->hashcode ) && offset == 0 ) {

            /* Profile is in the profile cache */

        int64_t hashcode; 

        memcpy(&hashcode, dp, sizeof(hashcode));

        icc_profile = gsicc_findprofile(hashcode, pis->icc_profile_cache);

        if (icc_profile == NULL)
            return gs_rethrow(-1, "cannot find profile in cache during clist read");

        rc_increment(icc_profile);

        return size;

    }

    if (icc_profile == NULL ){

        /* Allocate a new one and place in the cache.  Call to this should be with 
            the profile set to NULL. */

        icc_profile = gsicc_profile_new(NULL, pis->memory, NULL, 0);
        gsicc_add_profile(pis->icc_profile_cache, icc_profile, pis->memory);

        if (sizeof(profile_data) > size) {
                return gs_rethrow(-1, "insufficient memory to read icc header from clist");
        }

        memcpy(&profile_data, dp, sizeof(profile_data));
        dp += sizeof(profile_data);
        left -= sizeof(profile_data);
        offset += sizeof(profile_data);

        icc_profile->buffer_size = profile_data.buffer_size;
        icc_profile->data_cs = profile_data.data_cs;
        profile_data.default_match = icc_profile->default_match;
        profile_data.hash_is_valid = icc_profile->hash_is_valid;
        profile_data.hashcode = icc_profile->hashcode;
        profile_data.islab = icc_profile->islab;
        profile_data.num_comps = icc_profile->num_comps;
           
        for ( k = 0; k < profile_data.num_comps; k++ ){

            icc_profile->Range.ranges[k].rmax =
                profile_data.Range.ranges[k].rmax;
       
            icc_profile->Range.ranges[k].rmin =
                profile_data.Range.ranges[k].rmin;

        }

        icc_profile->name_length = 0;
        icc_profile->name = NULL;

        profile_size = icc_profile->buffer_size;

        if (icc_profile->buffer_size + sizeof(profile_data) > size) {
            return gs_rethrow(-1, "size mismatch clist reading icc profile");
        }

        /* Allocate the buffer space */

        if ( icc_profile->buffer == NULL ){
           icc_profile->buffer = gs_alloc_bytes(pis->memory, profile_size,
					        "gsicc_profile_clist_read");
        }

        if (icc_profile->buffer == NULL){
            return gs_rethrow(-1, "failed to allocate buffer for icc profile in clist");
        }

    }

    /* Now get the buffer */

    if (offset <= profile_size + sizeof(profile_data)) {

        int u = min(profile_size, left);
        
        memcpy( icc_profile->buffer + offset - sizeof(profile_data), dp, u);

        left -= u;
        offset += u;
        dp += u;

    }

    return size - left;
}



#if ICC_DUMP

/* Debug dump of ICC buffer data */

static void
dump_icc_buffer(int buffersize, char filename[],byte *Buffer)

{
    char full_file_name[50];
    FILE *fid;

    sprintf(full_file_name,"%d)%s_debug.icc",global_icc_index,filename);
    fid = fopen(full_file_name,"wb");

    fwrite(Buffer,sizeof(unsigned char),buffersize,fid);

    fclose(fid);

}


#endif
