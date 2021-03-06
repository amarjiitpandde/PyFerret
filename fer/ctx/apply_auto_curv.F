	SUBROUTINE APPLY_AUTO_CURV( full_var_name, cx, status )

*  This software was developed by the Thermal Modeling and Analysis
*  Project(TMAP) of the National Oceanographic and Atmospheric
*  Administration's (NOAA) Pacific Marine Environmental Lab(PMEL),
*  hereafter referred to as NOAA/PMEL/TMAP.
*
*  Access and use of this software shall impose the following
*  obligations and understandings on the user. The user is granted the
*  right, without any fee or cost, to use, copy, modify, alter, enhance
*  and distribute this software, and any derivative works thereof, and
*  its supporting documentation for any purpose whatsoever, provided
*  that this entire notice appears in all copies of the software,
*  derivative works and supporting documentation.  Further, the user
*  agrees to credit NOAA/PMEL/TMAP in any publications that result from
*  the use of this software or in any product that includes this
*  software. The names TMAP, NOAA and/or PMEL, however, may not be used
*  in any advertising or publicity to endorse or promote any products
*  or commercial entity unless specific written permission is obtained
*  from NOAA/PMEL/TMAP. The user also understands that NOAA/PMEL/TMAP
*  is not obligated to provide the user with any support, consulting,
*  training or assistance of any kind with regard to the use, operation
*  and performance of this software nor to provide the user with any
*  updates, revisions, new versions or "bug fixes".
*
*  THIS SOFTWARE IS PROVIDED BY NOAA/PMEL/TMAP "AS IS" AND ANY EXPRESS
*  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
*  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
*  ARE DISCLAIMED. IN NO EVENT SHALL NOAA/PMEL/TMAP BE LIABLE FOR ANY SPECIAL,
*  INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
*  RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
*  CONTRACT, NEGLIGENCE OR OTHER TORTUOUS ACTION, ARISING OUT OF OR IN
*  CONNECTION WITH THE ACCESS, USE OR PERFORMANCE OF THIS SOFTWARE. 
*
*  setup auto-curvilinear loading of aux vars and regridding
*      z = pt               ==> interpolation computed as a hidden curvi regrid
*      z = lo:hi@ave        ==> a compressing xform computed as a hidden curvi
*				regridding operation using pcwz averaging
* 
*      z = lo:hi:delta      ==> a curvi regrid via linear interpolation
*      z = lo:hi:delta@ave  ==> a curvi regrid via piececwise averaging

* V690 12/13 *sh* - hidden curvi regrids added 4/14
* V693+ 11/14 *sh* renaming 'sigma' as 'layerz' throughout
* V695+  3/15 *sh* added pauxrgrd_fmrc for GT(time2d)+1d and GF(time2d)=1d
* V698+  6/15 *sh* generalize single_pt applicable to all axes

	IMPLICIT NONE

	include 'tmap_dims.parm'
	include 'errmsg.parm'
	include 'ferret.parm'
	include 'xcontext.cmn'
	include 'xvariables.cmn'
	include 'xprog_state.cmn'
	include 'xtext_info.cmn'

* calling argument declarations
	INTEGER cx, status
	CHARACTER*(*) full_var_name

* internal variable declarations
	LOGICAL	single_pt
	INTEGER	idim, impl_grid
	REAL	range, midpt


* if any axes are set up for manual aux regridding, that takes precedence
	IF (cx_naux(cx) .GT. 0) RETURN

* loop over the implied curvilinear axes -- set them up for curvi regridding
	DO idim = 1, nferdims

* not a relevant axis?
	   IF (cx_aux_stat(idim,cx) .EQ. paux_stat_na) CYCLE

* if the region was specified by subscript suppresses implicit curvi regridding
	   IF (cx_given(idim,cx)
     .	 .AND. cx_by_ss(idim,cx)) CYCLE     ! ignore by_ss region limits

* by default assume that we will be regridding this axis
* a regrid implies that the aux axis status is "used"
	  cx_aux_stat(idim,cx) = paux_stat_used 

* single point Z-axis regrid?
	   single_pt = cx_lo_ww(idim,cx) .EQ. cx_hi_ww(idim,cx)
     .	     .OR.    ( cx_trans(idim,cx) .EQ. trans_average
     .           .AND. cx_delta(idim,cx) .EQ. unspecified_val8 )

* modify the xform or regridding xform to be an aux var regrid xform
	   IF     ( single_pt ) THEN   ! Z=pt or Z=lo:hi@ave
	      IF ( cx_trans(idim,cx) .EQ. trans_no_transform ) THEN
	         cx_regrid_trans(idim,cx) = pauxrgrd_linear
	      ELSEIF ( cx_trans(idim,cx) .EQ. trans_average ) THEN 
	         cx_regrid_trans(idim,cx) = pauxrgrd_pwlave
	      ELSE
	         GOTO 5200
	      ENDIF
	      cx_trans(idim,cx) = trans_no_transform
	   ELSEIF ( cx_regrid_trans(idim,cx) .EQ. prgrd_average 
     .	       .OR. cx_trans(idim,cx)        .EQ. trans_average ) THEN
!	      CALL WARN_OCCASIONALLY(
!     .		   'Box-averaging not available for layerz axis '//
!     .		   'regridding.'//pCR//
!     .		   '           Using piecewise linear averaging instead, '//
!     .		   'equivalent to gz(depth)=0:100@plave', 10, 101)
	       cx_regrid_trans(idim,cx) = pauxrgrd_pwlave
	       cx_trans       (idim,cx) = trans_no_transform
	   ELSEIF (cx_regrid_trans(idim,cx) .EQ. prgrd_linear_interp) THEN
	       cx_regrid_trans(idim,cx) = pauxrgrd_linear
	   ELSEIF (cx_trans       (idim,cx) .NE. trans_no_transform
     .	      .OR. cx_regrid_trans(idim,cx) .NE. unspecified_int4 ) THEN
	       GOTO 5200
	   ENDIF

* modify the interpretation of the region to accommodate the curvi operations
	   IF (cx_g_eq_regrid(idim)) THEN
* GZ=axis or GZ=lo:hi:delta
	      cx_aux_stat(idim,cx) = paux_stat_used  ! gz syntax sets itself up
	   ELSEIF (single_pt) THEN
* set up a "false regridding" by falsely setting cx_unstand_grid
* and setting cx_aux_stat as a flag that it is not a real regrid
* This is because we want to invoke the curvilinear regridding machinery
* but we want the result to remain on the original grid -- not unique 1pt grids
* Else layerz_temp[Z=50] and layerz_temp[Z=100] would get differing grids
	      cx_unstand_grid(cx) = .TRUE.
	      cx_aux_stat(idim,cx) = paux_stat_false_1pt_regrid
!	   ELSEIF (cx_regrid_trans(idim,cx) .EQ. pauxrgrd_pwlave
!     .	     .AND. cx_delta(idim,cx)        .EQ. unspecified_val8) THEN
!* Z=lo:hi@ave ==> regrid to a single point axis with these lo:hi end points
!	      range = cx_hi_ww(idim,cx) - cx_lo_ww(idim,cx)
!	      midpt = (cx_hi_ww(idim,cx) + cx_lo_ww(idim,cx)) / 2.
!	      cx_lo_ww(idim,cx) = midpt - range/2
!	      cx_hi_ww(idim,cx) = midpt + range/2
!	      cx_delta(idim,cx) = pauto_1pt_ax
	   ELSEIF ( cx_delta(idim,cx) .NE. unspecified_val8 ) THEN
* z=lo:hi:delta and z=lo:hi:delta@ave
	      CONTINUE
!	   ELSEIF (cx_given(idim,cx)
!     .	     .AND. cx_lo_ww(idim,cx) .EQ. cx_hi_ww(idim,cx)) THEN
!* Z=pt ==> regrid to a 1 point axis axis by interpolation
!	      cx_delta(idim,cx) = pauto_1pt_ax
	   ELSE
* no regridding after all -- just getting the aux vars for plotting
	      cx_aux_stat(idim,cx) = paux_stat_needed
	   ENDIF
	   cx_implicit_aux(cx) = .TRUE.
	   cx_naux(cx) = cx_naux(cx) + 1

* curvilinear and layerz regridding that are not single_pt imply a new target axis
* if it was not already created by GET_CONTEXT_MODS then create it here
	   IF (.NOT.(    single_pt
     .	            .OR. cx_aux_stat(idim,cx).EQ.paux_stat_needed
     .	            .OR. cx_unstand_grid(cx)
     .		    .OR. cx_has_impl_grid(cx))      ) THEN
	      CALL ALLO_GRID_TO_CX(cx,impl_grid,status)
	      IF ( status .NE. ferr_ok ) GOTO 5000
	      cx_unstand_grid(cx) = .TRUE.
	   ENDIF
	ENDDO

	status = ferr_ok
 5000	RETURN

* error exits
 5200	CALL ERRMSG( ferr_invalid_command, status,
     .	     'xform not (yet) supported on auxiliary variable regridding: '
     .		//full_var_name, *5000)

	END

