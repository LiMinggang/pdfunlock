/* Copyright (C) 1996, 1997 Aladdin Enterprises.  All rights reserved.
   Unauthorized use, copying, and/or distribution prohibited.
 */

/* pxerrors.c */
/* PCL XL error reporting */

#include "memory_.h"
#include "stdio_.h"		/* for sprintf */
#include "string_.h"
#include "gsmemory.h"
#include "gstypes.h"		/* for gsmatrix.h */
#include "gsccode.h"		/* for gxfont.h */
#include "gsmatrix.h"		/* for gsfont.h */
#include "gsstate.h"		/* for gsfont.h, gspath.h */
#include "gspaint.h"		/* for gs_erasepage */
#include "gscoord.h"
#include "gspath.h"
#include "gsutil.h"
#include "gxfixed.h"		/* for gxchar.h */
#include "gxchar.h"
#include "gxfont.h"
#include "scommon.h"		/* for pxparse.h */
#include "pxbfont.h"
#include "pxerrors.h"
#include "pxparse.h"
#include "pxptable.h"		/* for px_operator_names */
#include "pxstate.h"
#include "pxfont.h"

/* Imported operators */
px_operator_proc(pxEndPage);
px_operator_proc(pxSetPageDefaultCTM);

/* ---------------- Initialization ---------------- */

/* Create the error page font, and preallocate other structures so that */
/* printing an error message never needs to allocate storage. */
int
pxerrors_init(px_state_t *pxs)
{	px_font_t *pxfont = pl_alloc_font(pxs->memory, "error_page_font");
	gs_show_enum *penum = gs_show_enum_alloc(pxs->memory, pxs->pgs,
						 "error_page_show_enum");
	int code;

	if ( pxfont == 0 || penum == 0 )
	  code = gs_note_error(errorInsufficientMemory);
	else
	  { pxfont->storage = pxfsInternal;
	    pxfont->font_type = plft_Unicode; /* as good as any */
	    pxs->known_fonts_base_id = gs_next_ids(px_num_known_fonts + 1);
	    code =
	      px_define_font(pxfont, px_bitmap_font_header,
			     px_bitmap_font_header_size,
			     pxs->known_fonts_base_id + px_num_known_fonts,
			     pxs);
	    { const byte *cdata = px_bitmap_font_char_data;
	      while ( *cdata && code >= 0 )
		{ code = pl_font_add_glyph(pxfont, *cdata, cdata + 1);
		  ++cdata;
		  cdata = cdata + 16 +
		    ((uint16at(cdata + 10, true) + 7) >> 3) *
		    uint16at(cdata + 12, true);
		}
	    }
	  }
	if ( code < 0 )
	  { gs_free_object(pxs->memory, penum, "pxs->error_page_show_enum");
	    gs_free_object(pxs->memory, pxfont, "pxs->error_page_font");
	    return code;
	  }
	pxs->error_page_font = pxfont;
	pxs->error_page_show_enum = penum;
	return 0;
}

/* ---------------- Procedures ---------------- */

/* Record a warning. */
/* Return 1 if the warning table overflowed. */
/* If save_all is false, only remember the last warning with the same */
/* first word as this one. */
int
px_record_warning(const char *message, bool save_all, px_state_t *pxs)
{	uint end = pxs->warning_length;
	char *str = pxs->warnings + end;
	char *word_end = strchr(message, ' ');

	if ( end + strlen(message) + 1 > px_max_warning_message )
	  return 1;
	if ( !save_all && word_end )
	  { /* Delete any existing message of the same type. */
	    /* (There is at most one.) */
	    uint word_len = word_end - message;
	    char *next = pxs->warnings;
	    uint len1;

	    for ( ; next != str; next += len1 )
	      { len1 = strlen(next) + 1;
	        if ( len1 > word_len && !strncmp(next, message, word_len) )
		  { /* Delete the old message. */
		    memmove(next, next + len1, str - (next + len1));
		    str -= len1;
		    break;
		  }
	      }
	  }
	strcpy(str, message);
	pxs->warning_length = str + strlen(str) + 1 - pxs->warnings;
	return 0;
}

/* Generate a line of an error message starting at internal position N; */
/* return an updated value of N.  When done, return -1. */
int
px_error_message_line(char message[px_max_error_line+1], int N,
  const char *subsystem, int code, const px_parser_state_t *st,
  const px_state_t *pxs)
{	if ( N == 0 )
	  { strcpy(message, "PCL XL error\n");
	    return 1;
	  }
	if ( code == errorWarningsReported )
	  { /*
	     * Generate a line of warnings.
	     * 1 = first line, otherwise N = position in warnings buffer.
	     */
	    switch ( N )
	      {
	      case 1:
		N = 0;
		/* falls through */
	      default:
		if ( N == pxs->warning_length )
		  return -1;
		{ const char *str = pxs->warnings + N;
		  uint len = strlen(str);
		  uint warn_len;

		  strcpy(message, "    Warning:    ");
		  warn_len = strlen(message) + 1;
		  if ( len > px_max_error_line - warn_len )
		    { strncat(message, str, px_max_error_line - warn_len);
		      message[px_max_error_line - 1] = 0;
		    }
		  else
		    strcat(message, str);
		  strcat(message, "\n");
		  return N + len + 1;
		}
	      }
	  }
	else
	  { /* Generate the N'th line of an error message. */
	    char *end;
	    switch ( N )
	      {
	      case 1:
		sprintf(message, "    Subsystem:  %s\n", subsystem);
		break;
	      case 2:
		strcpy(message, "    Error:      ");
		{ char *end = message + strlen(message);
		  if ( pxs->error_line[0] )
		    { /* Ignore the error code, use the error line. */
		      int len = strlen(pxs->error_line);
		      int max_len = px_max_error_line - 2 - strlen(message);

		      if ( len <= max_len )
			strcpy(end, pxs->error_line);
		      else
			{ strncpy(end, pxs->error_line, max_len);
			  message[px_max_error_line - 1] = 0;
			}
		      strcat(end, "\n");
		    }
		  else if ( code >= px_error_first && code < px_error_next )
		    sprintf(end, "%s\n",
			    px_error_names[code - px_error_first]);
		  else
		    sprintf(end, "Internal error 0x%x\n", code);
		}
		break;
	      case 3:
		{ int last_operator = st->last_operator;
		  const char *oname;

		  strcpy(message, "    Operator:   ");
		  end = message + strlen(message);
		  if ( last_operator >= 0x40 && last_operator < 0xc0 &&
		       (oname = px_operator_names[last_operator - 0x40]) != 0
		     )
		    sprintf(end, "%s\n", oname);
		  else
		    sprintf(end, "0x%02x\n", last_operator);
		}
		break;
	      case 4:
		strcpy(message, "    Position:   ");
		end = message + strlen(message);
		if ( st->parent_operator_count )
		  sprintf(end, "%ld;%ld\n", st->parent_operator_count,
			  st->operator_count);
		else
		  sprintf(end, "%ld\n", st->operator_count);
		break;
	      default:
		return -1;
	      }
	    return N + 1;
	  }
}

/* Begin an error page.  Return the initial Y value. */
int
px_begin_error_page(px_state_t *pxs)
{	gs_state *pgs = pxs->pgs;

	if ( pxs->have_page )
	  { px_args_t args;
	    args.pv[0] = 0;
	    pxEndPage(&args, pxs);
	  }
	gs_initgraphics(pgs);
	gs_erasepage(pgs);
	/* Don't call pxSetPageDefaultCTM -- we don't want rotation or */
	/* unusual Units of Measure -- but do invert the Y axis. */
	/*pxSetPageDefaultCTM(NULL, pxs);*/
	{
	    gs_point pt;
	    px_get_default_media_size(pxs, &pt);
	    gs_translate(pgs, 0.0, pt.y);
	    gs_scale(pgs, 1.0, -1.0);
	    gs_setfont(pgs, (gs_font *)pxs->error_page_font->pfont);
	    return 90;
	}
}

/* Print a message on an error page. */
/* Return the updated Y value. */
int
px_error_page_show(const char *message, int ytop, px_state_t *pxs)
{	gs_state *pgs = pxs->pgs;
	int y = ytop;
	const char *m = message;
	const char *p;
	gs_show_enum *penum = pxs->error_page_show_enum;
	/* Normalize for a 10-point font. */
#define point_size 10.0
	double scale = 72.0 / px_bitmap_font_resolution *
	  point_size / px_bitmap_font_point_size;
	int code = 0;

	/* Peel off the next line and display it. */
	for ( p = m; ; m = ++p )
	  { while ( *p != 0 && *p != '\n' )
	      ++p;
	    gs_moveto(pgs, 36.0, y);
	    gs_scale(pgs, scale, scale);
	    code = gs_show_n_init(penum, pgs, m, p - m);
	    if ( code >= 0 )
	      { code = gs_show_next(penum);
		if ( code > 0 )
		  code = gs_note_error(errorBadFontData);	/* shouldn't happen! */
	      }
	    if ( code < 0 )
	      gs_show_enum_release(penum, NULL);
	    gs_scale(pgs, 1 / scale, 1 / scale);
	    y += point_size * 8 / 5;
	    if ( !*p || !p[1] )
	      break;
	  }
	return (code < 0 ? code : y);
}

/* Reset the warning table. */
void
px_reset_errors(px_state_t *pxs)
{	pxs->error_line[0] = 0;
	pxs->warning_length = 0;
}

/* ---------------- Error names ---------------- */

#undef pxerrors_INCLUDED
#define INCLUDE_ERROR_NAMES
#include "pxerrors.h"
#undef INCLUDE_ERROR_NAMES
