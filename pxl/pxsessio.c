/* Copyright (C) 1996, 1997 Aladdin Enterprises.  All rights reserved.
   Unauthorized use, copying, and/or distribution prohibited.
 */

/* pxsessio.c */
/* PCL XL session operators */

#include "math_.h"		/* for fabs */
#include "stdio_.h"
#include "pxoper.h"
#include "pxstate.h"
#include "pxfont.h"		/* for px_free_font */
#include "gschar.h"
#include "gscoord.h"
#include "gserrors.h"		/* for gs_error_undefined */
#include "gspaint.h"
#include "gsparam.h"
#include "gsstate.h"
#include "gxfixed.h"
#include "gxfcache.h"
#include "gxdevice.h"

/* Imported operators */
px_operator_proc(pxCloseDataSource);
px_operator_proc(pxNewPath);
px_operator_proc(pxPopGS);
px_operator_proc(pxPushGS);
px_operator_proc(pxSetHalftoneMethod);
px_operator_proc(pxSetPageDefaultCTM);

/*
 * Define the known paper sizes and unprintable margins.  For convenience,
 * we define this in terms of 300 dpi pixels, in portrait orientation.  This
 * table should obviously be device-dependent.
 */
#define media_size_scale (72.0 / 300.0)
typedef struct px_media_s {
  pxeMediaSize_t ms_enum;
  short width, height;
  short m_left, m_top, m_right, m_bottom;
} px_media_t;
#define m_default 50, 50, 50, 50
#define m_data(ms_enum, res, width, height)\
  {ms_enum, width * 300 / (res), height * 300 / (res), m_default},
private px_media_t known_media[] = {
  px_enumerate_media(m_data)
  /* The list ends with a comma, so add a dummy entry */
  /* that can't be matched because its key is a duplicate. */
  {eLetterPaper}
};
#undef m_data
#undef m_default

/* Define the mapping from the Measure enumeration to points. */
private const double measure_to_points[] = pxeMeasure_to_points;

/* ---------------- Internal procedures ---------------- */

/* return the default media set up in the XL state */
private px_media_t * 
px_get_default_media(px_state_t *pxs)
{
    int i;
    for (i = 0; i < countof(known_media); i++)
	if ( known_media[i].ms_enum == pxs->media_size )
	    return &known_media[i];
    /* shouldn't get here but just in case we return letter. */
    return &known_media[0];
}

 void
px_get_default_media_size(px_state_t *pxs, gs_point *pt)
{
    px_media_t *media = px_get_default_media(pxs);
    pt->x = media->width * media_size_scale;
    pt->y = media->height * media_size_scale;
}

/* Finish putting one device parameter. */
private int
px_put1(gx_device *dev, gs_c_param_list *plist, int ecode)
{	int code = ecode;
	if ( code >= 0 )
	  { gs_c_param_list_read(plist);
	    code = gs_putdeviceparams(dev, (gs_param_list *)plist);
	  }
	gs_c_param_list_release(plist);
	return (code == 0 || code == gs_error_undefined ? ecode : code);
}

/* Adjust one scale factor to an integral value if we can. */
private double
px_adjust_scale(double value, double extent)
{	/* If we can make the value an integer with a total error */
	/* of less than 1/2 pixel over the entire page, we do it. */
	double int_value = floor(value + 0.5);

	return (fabs((int_value - value) * extent) < 0.5 ? int_value : value);
}

/* Clean up at the end of a page, but before rendering. */
private void
px_end_page_cleanup(px_state_t *pxs)
{	px_dict_release(&pxs->page_pattern_dict);
	/* Clean up stray gstate information. */
	while ( pxs->pxgs->stack_depth > 0 )
	  pxPopGS(NULL, pxs);
	/* Pop an extra time to mirror the push in BeginPage. */
	pxs->pxgs->stack_depth++;
	pxPopGS(NULL, pxs);
	pxNewPath(NULL, pxs);
	px_purge_pattern_cache(pxs, ePagePattern);
}

/* Purge characters in non-built-in fonts. */
private bool
match_temporary_glyph(cached_char *cc, void *vpxs)
{	const cached_fm_pair *pair = cc->pair;
	px_state_t *pxs = vpxs;

	return (uid_is_UniqueID(&pair->UID) &&
		pair->UID.id > pxs->known_fonts_base_id + px_num_known_fonts);
}

/* Clean up at the end of a session. */
private void
px_end_session_cleanup(px_state_t *pxs)
{	if ( pxs->data_source_open )
	  pxCloseDataSource(NULL, pxs);
	gx_purge_selected_cached_chars(pxs->font_dir, match_temporary_glyph,
				       pxs);
	px_dict_release(&pxs->font_dict);
	px_dict_release(&pxs->session_pattern_dict);
	px_purge_pattern_cache(pxs, eSessionPattern);
	/* We believe that streams do *not* persist across sessions.... */
	px_dict_release(&pxs->stream_dict);
}

/* ---------------- Non-operator procedures ---------------- */

/* Clean up after an error or UEL. */
void
px_state_cleanup(px_state_t *pxs)
{	px_end_page_cleanup(pxs);
	px_end_session_cleanup(pxs);
	pxs->have_page = false;
}

/* ---------------- Operators ---------------- */

const byte apxBeginSession[] = {
  pxaMeasure, pxaUnitsPerMeasure, 0,
  pxaErrorReport, 0
};
int
pxBeginSession(px_args_t *par, px_state_t *pxs)
{	pxs->measure = par->pv[0]->value.i;
	pxs->units_per_measure.x = real_value(par->pv[1], 0);
	pxs->units_per_measure.y = real_value(par->pv[1], 1);
	pxs->error_report = (par->pv[2] ? par->pv[2]->value.i : eNoReporting);
	px_dict_init(&pxs->session_pattern_dict, pxs->memory, px_free_pattern);
	/* Set media parameters to device defaults, in case BeginPage */
	/* doesn't specify valid ones. */
	/* This is obviously device-dependent. */
	/* get the pjl state */
	{
	    char* pjl_psize = pjl_get_envvar(pxs->pjls, "paper");
	    /* NB.  We are not sure about the interaction of pjl's
               wide a4 commands so we don't attempt to implement
               it. */
	    /* bool pjl_widea4 = pjl_compare(pjl_get_envvar(pxs->pjls, "widea4"), "no"); */
	    int pjl_copies = pjl_vartoi(pjl_get_envvar(pxs->pjls, "copies"));
	    bool pjl_duplex = pjl_compare(pjl_get_envvar(pxs->pjls, "duplex"), "off");
	    bool pjl_bindshort = pjl_compare(pjl_get_envvar(pxs->pjls, "binding"), "longedge");
	    bool pjl_manualfeed = pjl_compare(pjl_get_envvar(pxs->pjls, "manualfeed"), "off");
	    /* table to map pjl paper type strings to pxl enums */
	    private const struct {
		const char *pname;
		pxeMediaSize_t paper_enum;
	    } pjl_paper_sizes[] = {
		{ "letter", eLetterPaper },
		{ "legal", eLegalPaper },
		{ "a4", eA4Paper },
                { "executive", eExecPaper },
		{ "ledger", eLedgerPaper },
		{ "a3", eA3Paper },        
		{ "com10", eCOM10Envelope },
		{ "monarch", eMonarchEnvelope },
		{ "c5", eC5Envelope },
		{ "dl", eDLEnvelope },
		{ "jisb4", eJB4Paper },
		{ "jisb5", eJB5Paper },       
		{ "b5", eB5Envelope },
		{ "jpost", eJPostcard },
		{ "jpostd", eJDoublePostcard },
		{ "a5", eA5Paper },        
		{ "a6", eA6Paper },
		{ "jisb6", eJB6Paper },       
	    };
	    int i;
	    for (i = 0; i < countof(pjl_paper_sizes); i++)
		if ( !pjl_compare(pjl_psize, pjl_paper_sizes[i].pname) ) {
		    pxs->media_size = pjl_paper_sizes[i].paper_enum;
		    break;
		}
	    pxs->media_source = (pjl_manualfeed ? eManualFeed : eDefaultSource);
	    pxs->duplex = pjl_duplex;
	    pxs->duplex_page_mode = (pjl_bindshort ? eDuplexHorizontalBinding :
				     eDuplexVerticalBinding);
	    pxs->copies = pjl_copies;
	    pxs->media_destination = eDefaultDestination;
	    px_dict_init(&pxs->font_dict, pxs->memory, px_free_font);
	}
	return 0;
}

const byte apxEndSession[] = {0, 0};
int
pxEndSession(px_args_t *par, px_state_t *pxs)
{	px_end_session_cleanup(pxs);
	if ( pxs->warning_length )
	  return_error(errorWarningsReported);
	return 0;
}

/**** NOTE: MediaDestination and MediaType are not documented by H-P. ****/
/* We are guessing that they are enumerations, like MediaSize/Source. */
const byte apxBeginPage[] = {
  pxaOrientation, 0,
  pxaMediaSource, pxaMediaSize, pxaCustomMediaSize, pxaCustomMediaSizeUnits,
  pxaSimplexPageMode, pxaDuplexPageMode, pxaDuplexPageSide,
  pxaMediaDestination, pxaMediaType, 0
};
int
pxBeginPage(px_args_t *par, px_state_t *pxs)
{	gs_state *pgs = pxs->pgs;
	gx_device *dev = gs_currentdevice(pgs);
	const px_media_t *pm;
	gs_point page_size_pixels;
	gs_point media_size;

	/* Check parameter presence for legal combinations. */
	if ( par->pv[2] )
	  { if ( par->pv[3] || par->pv[4] )
	      return_error(errorIllegalAttributeCombination);
	  }
	else if ( par->pv[3] && par->pv[4] )
	  { if ( par->pv[2] )
	      return_error(errorIllegalAttributeCombination);
	  }
	else
	  return_error(errorMissingAttribute);
	if ( par->pv[5] )
	  { if ( par->pv[6] || par->pv[7] )
	      return_error(errorIllegalAttributeCombination);
	  }
	else if ( par->pv[6] )
	  { if ( par->pv[5] )
	      return_error(errorIllegalAttributeCombination);
	  }

	/* Copy parameters to the PCL XL state. */
	{ /* For some reason, invalid Orientations only produce a warning. */
	  integer orientation = par->pv[0]->value.i;
	  if ( orientation < 0 || orientation >= pxeOrientation_next )
	    { px_record_warning("IllegalOrientation", true, pxs);
	      orientation = ePortraitOrientation;
	    }
	  pxs->orientation = (pxeOrientation_t)orientation;
	}
	if ( par->pv[1] )
	  pxs->media_source = par->pv[1]->value.i;
	if ( par->pv[2] )
	  { pxeMediaSize_t ms_enum = par->pv[2]->value.i;
	    int i;

	    for ( pm = known_media, i = 0; i < countof(known_media);
		  ++pm, ++i
		)
	      if ( pm->ms_enum == ms_enum )
		break;
	    if ( i == countof(known_media) )
	      { /* No match, select default media. */
		pm = px_get_default_media(pxs);
		px_record_warning("IllegalMediaSize", false, pxs);
	      }
	    pxs->media_size = pm->ms_enum;
	    media_size.x = pm->width * media_size_scale;
	    media_size.y = pm->height * media_size_scale;
	  }
	else
	  { double scale = measure_to_points[par->pv[4]->value.i];
	    media_size.x = real_value(par->pv[3], 0) * scale;
	    media_size.y = real_value(par->pv[3], 1) * scale;
	    /*
	     * Assume the unprintable margins for custom media are the same
	     * as for the default media.  This may not be right.
	     */
	    pm = px_get_default_media(pxs);
	  }
	if ( par->pv[5] )
	  { pxs->duplex = false;
	  }
	else if ( par->pv[6] )
	  { pxs->duplex = true;
	    pxs->duplex_page_mode = par->pv[6]->value.i;
	    if ( par->pv[7] )
	      pxs->duplex_back_side = (par->pv[7]->value.i == eBackMediaSide);
	  }
	if ( par->pv[8] )
	  pxs->media_destination = par->pv[8]->value.i;
	if ( par->pv[9] )
	  pxs->media_type = par->pv[9]->value.i;

	/* Pass the media parameters to the device. */
	{ gs_memory_t *mem = pxs->memory;
	  gs_c_param_list list;
#define plist ((gs_param_list *)&list)
	  gs_param_float_array fa;
	  float fv[4];
	  int iv;
	  bool bv;
	  int ecode = 0;
	  int code;

	  fa.data = fv;
	  fa.persistent = false;

	  gs_c_param_list_write(&list, mem);
	  iv = pxs->orientation;	/* might not be an int */
	  code = param_write_int(plist, "Orientation", &iv);
	  ecode = px_put1(dev, &list, ecode);

	  gs_c_param_list_write(&list, mem);
	  fv[0] = media_size.x;
	  fv[1] = media_size.y;
	  fa.size = 2;
	  code = param_write_float_array(plist, ".MediaSize", &fa);
	  ecode = px_put1(dev, &list, ecode);

	  gs_c_param_list_write(&list, mem);

	  /* be careful not to set up a clipping region beyond the
             physical capabilites of the driver.  It seems like
             pm->m_top and pm->m_bottom are reversed but this is
             consistant with the way it was before. */
	  fv[0] = max((dev->HWMargins[0]), (pm->m_left * media_size_scale));
	  fv[1] = max((dev->HWMargins[1]), (pm->m_top * media_size_scale));
	  fv[2] = max((dev->HWMargins[2]), (pm->m_right * media_size_scale));
	  fv[3] = max((dev->HWMargins[3]), (pm->m_bottom * media_size_scale));
	  fa.size = 4;
	  code = param_write_float_array(plist, ".HWMargins", &fa);
	  ecode = px_put1(dev, &list, ecode);

	  /* Set the mis-named "Margins" (actually the offset on the page) */
	  /* to zero. */
	  gs_c_param_list_write(&list, mem);
	  fv[0] = 0;
	  fv[1] = 0;
	  fa.size = 2;
	  code = param_write_float_array(plist, "Margins", &fa);
	  ecode = px_put1(dev, &list, ecode);

	  iv = pxs->media_source;	/* might not be an int */
	  if ( iv < 0 || iv >= pxeMediaSource_next )
	    px_record_warning("IllegalMediaSource", false, pxs);
	  else
	    { gs_c_param_list_write(&list, mem);
	      code = param_write_int(plist, ".MediaSource", &iv);
	      ecode = px_put1(dev, &list, ecode);
	    }

	  gs_c_param_list_write(&list, mem);
	  code = param_write_bool(plist, "Duplex", &pxs->duplex);
	  ecode = px_put1(dev, &list, ecode);

	  gs_c_param_list_write(&list, mem);
	  bv = pxs->duplex_page_mode == eDuplexHorizontalBinding;
	  code = param_write_bool(plist, "Tumble", &bv);
	  ecode = px_put1(dev, &list, ecode);

	  gs_c_param_list_write(&list, mem);
	  bv = !pxs->duplex_back_side;
	  code = param_write_bool(plist, "FirstSide", &bv);
	  ecode = px_put1(dev, &list, ecode);

	  gs_c_param_list_write(&list, mem);
	  iv = pxs->media_destination;	/* might not be an int */
	  code = param_write_int(plist, ".MediaDestination", &iv);
	  ecode = px_put1(dev, &list, ecode);

	  gs_c_param_list_write(&list, mem);
	  iv = pxs->media_type;		/* might not be an int */
	  code = param_write_int(plist, ".MediaType", &iv);
	  ecode = px_put1(dev, &list, ecode);

	  /*
	   * We aren't sure what to do if the device rejects the parameter
	   * value....
	   */
	  switch ( ecode )
	    {
	    case 1:
	      code = gs_setdevice(pgs, dev);
	      if ( code < 0 )
		return code;
	    case 0:
	      break;
	    default:
	      return_error(errorIllegalAttributeValue);
	    }
#undef plist
	}
	{ int code;

	  px_initgraphics(pxs);
	  gs_dtransform(pgs, media_size.x, media_size.y,
			&page_size_pixels);
	  { /*
	     * Put the origin at the upper left corner of the page;
	     * also account for the orientation.
	     */
	    gs_matrix orient;

	    orient.xx = orient.xy = orient.yx = orient.yy =
	      orient.tx = orient.ty = 0;
	    switch ( pxs->orientation )
	      {
	      case ePortraitOrientation:
		code = gs_translate(pgs, 0.0, media_size.y);
		orient.xx = 1, orient.yy = -1;
		break;
	      case eLandscapeOrientation:
		code = 0;
		orient.xy = 1, orient.yx = 1;
		break;
	      case eReversePortrait:
		code = gs_translate(pgs, media_size.x, 0);
		orient.xx = -1, orient.yy = 1;
		break;
	      case eReverseLandscape:
		code = gs_translate(pgs, media_size.x, media_size.y);
		orient.xy = -1, orient.yx = -1;
		break;
	      default:			/* can't happen */
		return_error(errorIllegalAttributeValue);
	      }
	    if ( code < 0 ||
		 (code = gs_concat(pgs, &orient)) < 0
	       )
	      return code;
	  }
	  { /* Scale according to session parameters. */
	    /* If we can make the scale integral safely, we do. */
	    double scale = measure_to_points[pxs->measure];
	    gs_matrix mat;

	    if ( (code = gs_scale(pgs, scale / pxs->units_per_measure.x,
				  scale / pxs->units_per_measure.y)) < 0
	       )
	      return code;
	    gs_currentmatrix(pgs, &mat);
	    mat.xx = px_adjust_scale(mat.xx, page_size_pixels.x);
	    mat.xy = px_adjust_scale(mat.xy, page_size_pixels.y);
	    mat.yx = px_adjust_scale(mat.yx, page_size_pixels.x);
	    mat.yy = px_adjust_scale(mat.yy, page_size_pixels.y);
	    gs_setmatrix(pgs, &mat);
	    pxs->initial_matrix = mat;
	  }
	}
	  { /*
	     * Set the default halftone method.  We have to do this here,
	     * rather than earlier, so that the origin is set correctly.
	     */
	    px_args_t args;
	    px_value_t device_matrix;

	    args.pv[0] = 0;	/* DitherOrigin */
	    args.pv[1] = &device_matrix;	/* DeviceMatrix */
	      device_matrix.type = pxd_scalar | pxd_ubyte;
	      device_matrix.value.i = eDeviceBest;
	    args.pv[2] = 0;	/* DitherMatrixDataType */
	    args.pv[3] = 0;	/* DitherMatrixSize */
	    args.pv[4] = 0;	/* DitherMatrixDepth */
	    pxSetHalftoneMethod(&args, pxs);
	  }
	/* Initialize other parts of the PCL XL state. */
	px_dict_init(&pxs->page_pattern_dict, pxs->memory, px_free_pattern);
	gs_erasepage(pgs);
	pxs->have_page = false;
	/* Make sure there is a legitimate halftone installed. */
	{ int code = px_set_halftone(pxs);
	  if ( code < 0 )
	    return code;
	}
	/*
	 * Do a gsave so we can be sure to get rid of all page-related
	 * state at the end of the page, but make sure PopGS doesn't pop
	 * this state from the stack.
	 */
	{ int code = pxPushGS(NULL, pxs);
	  if ( code < 0 )
	    return code;
	  pxs->pxgs->stack_depth--;
	  return code;
	}
}

const byte apxEndPage[] = {
  0,
  pxaPageCopies, 0
};
int
pxEndPage(px_args_t *par, px_state_t *pxs)
{	px_end_page_cleanup(pxs);
	(*pxs->end_page)(pxs, (par->pv[0] ? par->pv[0]->value.i : pxs->copies), 1);
	pxs->have_page = false;
	return 0;
}
/* The default end-page procedure just calls the device procedure. */
int
px_default_end_page(px_state_t *pxs, int num_copies, int flush)
{	return gs_output_page(pxs->pgs, num_copies, flush);
}

const byte apxComment[] = {
  0,
  pxaCommentData, 0
};
int
pxComment(px_args_t *par, px_state_t *pxs)
{	return 0;
}

const byte apxOpenDataSource[] = {
  pxaSourceType, pxaDataOrg, 0, 0
};
int
pxOpenDataSource(px_args_t *par, px_state_t *pxs)
{	if ( pxs->data_source_open )
	  return_error(errorDataSourceNotClosed);
	pxs->data_source_open = true;
	pxs->data_source_big_endian =
	  par->pv[1]->value.i == eBinaryHighByteFirst;
	return 0;
}

const byte apxCloseDataSource[] = {0, 0};
int
pxCloseDataSource(px_args_t *par, px_state_t *pxs)
{	pxs->data_source_open = false;
	return 0;
}
