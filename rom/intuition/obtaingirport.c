/*
    (C) 1995-99 AROS - The Amiga Research OS
    $Id$

    Desc: Intuition Function ObtainGIRPort()
    Lang: english
*/
#include "intuition_intern.h"
#include <proto/graphics.h>

/*****************************************************************************

    NAME */
#include <graphics/rastport.h>
#include <intuition/cghooks.h>
#include <proto/intuition.h>

	AROS_LH1(struct RastPort *, ObtainGIRPort,

/*  SYNOPSIS */
	AROS_LHA(struct GadgetInfo *, gInfo, A0),

/*  LOCATION */
	struct IntuitionBase *, IntuitionBase, 93, Intuition)

/*  FUNCTION
	This function sets up a RastPort for exclusive use by custom
	gadget hook routines. Call this function each time a hook
	routine needs to render into the gadget and ReleaseGIRPort()
	immediately afterwards.

    INPUTS
	gInfo - Pointer to GadgetInfo structure, as passed to each
	custom gadget hook function.

    RESULT
	Pointer to a RastPort you can render to. NULL if you aren't
	allowed to render into this gadget.

    NOTES
	If a routine passes a RastPort, eg. GM_RENDER, ObtainGIRPort()
	needn't be called.

    EXAMPLE

    BUGS

    SEE ALSO
	ReleaseGIRPort()

    INTERNALS

    HISTORY
	29-10-95    digulla automatically created from
			    intuition_lib.fd and clib/intuition_protos.h

*****************************************************************************/
{
    AROS_LIBFUNC_INIT
    AROS_LIBBASE_EXT_DECL(struct IntuitionBase *,IntuitionBase)

    return CloneRastPort (gInfo->gi_RastPort);

    AROS_LIBFUNC_EXIT
} /* ObtainGIRPort */
