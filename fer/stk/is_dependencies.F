	SUBROUTINE IS_DEPENDENCIES( *, *, work, status )

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
*
* this routine is derived from IS_UVAR_GRID
* see that routine for a discussion of how it is implemented
* instead of evaluating the grid of a uvar, this routine traces through the
* same path and reports out the dependencies
* It also handles dependencies created by
*  o auxiliary variables used in uaxvar regridding
*  o aggregated variables created using DEFINE AGGREGATION

* programmer - steve hankin
* NOAA/PMEL, Seattle, WA - Tropical Modeling and Analysis Program
*
* V685+ 9/13 *sh*  

* calling argument declarations:
	INTEGER	status
	INTEGER work(*)
* normal return is for error conditions
* exit #1 is to request dependencies of a component ( stack has been pushed )
* exit #2 is to signal that the dependencies of a uvar have been traced

* internal variable declarations:
	LOGICAL	ITSA_GCVAR, its_gc, 
     .		get_aux_vars, delayed_by_auxvars, desist
	INTEGER TM_LENSTR1, GCF_PARENT_UVAR, GCF_PARENT_CHAR, 
     .		NCF_GET_AGG_COUNT,
     .		iaux, auxcnt, hi_cx, lo_cx, aux_cx, start, iend,
     .		i, dset, uvar, item, itype, idim, nitem, var, len_name, 
     .          memb_cx, agg_dset, agg_len, var_status,
     .		imemb, fer_uvar, memb_dset, memb_grid, iline, nv

	CHARACTER VAR_CODE*128, name*512, varcode*128

#include "netcdf.inc"
	include	'tmap_errors.parm'
	include 'tmap_dims.parm'
	include	'ferret.parm'
	include	'errmsg.parm'
	include	'interp_stack.parm'
	include	'xcontext.cmn'
	include	'xvariables.cmn'
	include	'xdependency_tree.cmn'

*        ... interpretation stack setup at the time this is called ...
*	stack level		stack contents
*	isp+1	<--	level to get grids for components
*	 isp	<--	current level: evaluate uvar or var/auxvar dependencies

* --- end of introductory code ---

* initialize
	lo_cx  = is_cx( isp )
	uvar   = is_uvar( isp )
	dset   = cx_data_set( lo_cx )
	nitem  = uvar_num_items( uvar )
	its_gc = ITSA_GCVAR(uvar)
!	has_uvar_gc = has_uvar_gc .OR. its_gc  ! for repl_exprns fixing bug 1523
* if aux var dependencies have just been reported, then we have to yet to 
* report user variable dependencies
* e.g.	in
*   LET V = a + b
*   SHOW VAR/DEP v[gz(depth)=zpts]
* after reporting that V depends on depth, we need to report on "V=a+b"
	delayed_by_auxvars = is_sum(isp) .NE. 0
	IF (delayed_by_auxvars) is_phase(isp) = is_phase(isp) - 1

****
* Handle auxiliary variables
	get_aux_vars = is_act(isp) .EQ. isact_auxvar_regrid
	IF  ( get_aux_vars ) THEN
 10	   is_phase(isp) = is_phase(isp) + 1
	   iaux = is_phase(isp)
	   IF (iaux .GT. cx_naux(lo_cx) ) THEN
	      is_sum(isp-1) = 1  ! signal completion of aux vars
	      GOTO 1000  ! we have finished reporting aux vars
	   ENDIF
	   CALL STACK_PTR_UP( cx_stack_ptr, max_context, status )
	   IF ( status .NE. ferr_ok ) GOTO 5000
	   aux_cx = cx_stack_ptr
* ... in GET_VAR_CONTEXT valid_aux_var was set. Tells if aux varname is known
	   IF (.NOT. cx_valid_aux(iaux, lo_cx) ) THEN
* ... find the idim value where this aux var info is stored
	      auxcnt = 0
	      DO 12 idim = 1, nferdims
	         IF ( cx_aux_arg_start(idim, hi_cx)
     .		 .NE. unspecified_int4)  auxcnt = auxcnt + 1
	         IF (auxcnt .EQ. iaux) GO to 14
 12	      CONTINUE
	      CALL ERRMSG( ferr_internal, 'is_dependencies', *5000)
* ... extract the start and end of the text string for the unknown auxvar
 14	      cx_variable(aux_cx) = cx_aux_arg_start(idim, hi_cx) 
	      cx_category(aux_cx) = cx_aux_arg_end (idim, hi_cx) 
	      status = ferr_unknown_auxvar
	   ELSE
* ... get full context for a known auxiliary variable
	      CALL GET_AUX_VAR_CONTEXT( hi_cx, aux_cx, iaux, status )
	      IF ( status .NE. ferr_ok ) GOTO 5000
 	   ENDIF
	   CALL REPORT_DEPENDENCY( aux_cx, isp, uvar, item,
     .				   work, status, desist)
           IF (.NOT.desist
     .    .AND. cx_category(aux_cx) .EQ. cat_user_var) THEN
* ... if its a uvar go show dependencies INSIDE of the aux variable definition
              CALL PUSH_INTERP_STACK( isp, status )
              IF ( status .NE. ferr_ok ) RETURN
              is_cx   ( isp ) = aux_cx
              is_uvar ( isp ) = cx_variable( aux_cx )
              is_act  ( isp ) = isact_algebra
              RETURN 1
	   ELSE
	      cx_stack_ptr = cx_stack_ptr - 1
	      GOTO 10	! do next aux var
           ENDIF
	ENDIF

****
* Handle aggregate variables
	IF ( is_act(isp) .EQ. isact_aggregate_gather ) THEN
	   agg_dset = dset
	   status = NCF_GET_AGG_COUNT (agg_dset, agg_len)
	   var = cx_variable(lo_cx)
 20	   is_phase(isp) = is_phase(isp) + 1
	   imemb = is_phase(isp)
	   IF (imemb .GT. agg_len ) THEN
	      GOTO 1000  ! we have finished reporting aux vars
	   ENDIF
* .. set up a stack level w/ info about the next aggregation member
	   CALL STACK_PTR_UP( cx_stack_ptr, max_context, status )
	   IF ( status .NE. ferr_ok ) GOTO 5000
	   memb_cx = cx_stack_ptr
	   CALL TRANSFER_CONTEXT(lo_cx, memb_cx)
	   varcode = VAR_CODE(cat_aggregate_var, var)
	   CALL CD_GET_AGG_VAR_INFO (agg_dset, varcode, imemb, 
     .            fer_uvar, memb_dset, memb_grid, iline, nv, status)
	   cx_category(memb_cx) = fer_uvar
	   cx_data_set(memb_cx) = memb_dset
	   cx_variable(memb_cx) = nv
	   status = ferr_ok
	   CALL REPORT_DEPENDENCY(memb_cx, isp, uvar, item,
     .				  work, status, desist )
           IF (.NOT.desist
     .	  .AND. cx_category(memb_cx) .EQ. cat_user_var) THEN
* ... if its a uvar go show dependencies
              CALL PUSH_INTERP_STACK( isp, status )
              IF ( status .NE. ferr_ok ) RETURN
              is_cx   ( isp ) = memb_cx
              is_uvar ( isp ) = cx_variable( memb_cx )
              is_act  ( isp ) = isact_algebra
              RETURN 1
	   ELSE
	      cx_stack_ptr = cx_stack_ptr - 1
	      GOTO 20	! do next aggregation member
           ENDIF
	ENDIF


****
* Handle user-defined variable (uvar)
* is_act(isp) is equal to isact_aggregate_gather
* process the next item in the expression from the uvar buffer (last to first)
 100	is_phase(isp) = is_phase(isp) + 1
* ... for showing dependencies traverse in natural, forward order
!	item = nitem - is_phase(isp)	+ 1	! reverse-ordered pointer
	item = is_phase(isp)
	IF ( item .GT. nitem ) GOTO 1000	! have processed entire def'n
	itype = uvar_item_type ( item, uvar )
	start = uvar_item_start( item, uvar )
	iend   = uvar_item_end  ( item, uvar )
	IF     ( itype .EQ. alg_pseudo_var  ) THEN
	   GOTO 100
	ELSEIF ( itype .EQ. alg_const_var   ) THEN
	   GOTO 100
	ELSEIF ( itype .EQ. alg_child_var   ) THEN
	   GOTO 300
	ELSEIF ( itype .EQ. alg_variable    ) THEN
	   GOTO 305
	ELSEIF ( itype .EQ. alg_attrib_val  ) THEN
	   GOTO 100
	ELSEIF ( itype .EQ. alg_string      ) THEN
	   GOTO 100
	ELSEIF ( itype .EQ. alg_constant    ) THEN
	   GOTO 100
	ELSEIF ( itype .EQ. alg_counter_var   ) THEN
	   GOTO 100
	ELSEIF ( itype .EQ. alg_then_marker ) THEN  ! marks where the IF was
	   GOTO 100
	ELSE
	   GOTO 100
	ENDIF

* ==================================

 300	CALL GCF_CHILD_VAR_NAME( GCF_PARENT_UVAR(uvar),
     .				 GCF_PARENT_CHAR(uvar,start), name )
	len_name = TM_LENSTR1(name)
	GOTO 310

* variable: parse name, get full expression context
 305	name = uvar_text(uvar)(start:iend)
	len_name = iend-start+1
 310	CALL STACK_PTR_UP( cx_stack_ptr, max_context, status )
	IF ( status .NE. ferr_ok ) GOTO 5000
	hi_cx = cx_stack_ptr
	CALL GET_VAR_CONTEXT (  name(:len_name),
     .				lo_cx,
     .				hi_cx,
     .				var_status )

* have we returned here after reporting auxvar dependencies?
* if so we have already reported this variable
	IF (delayed_by_auxvars) THEN
	   delayed_by_auxvars = .FALSE.
	   is_sum(isp) = 0
	   GOTO 900
	ENDIF

* report the variable just parsed -- known or unknown or with syntax errors
*   note that auxvar errors are dealt with in the auxvar block above
	IF ( var_status .NE. ferr_unknown_auxvar ) THEN
	   CALL REPORT_DEPENDENCY( hi_cx, isp, uvar, item,
     .				   work, var_status, desist )
	ENDIF

* if the variable used auxiliary variables for regridding set up new isp
* level and go report them (before reporting on uvar dependencies)
	IF  ( .NOT.desist
     .	.AND. cx_unstand_grid(hi_cx)
     .	.AND. cx_naux(hi_cx) .GT. 0 ) THEN
	   CALL PUSH_INTERP_STACK( isp, status )
	   IF ( status .NE. ferr_ok ) RETURN
	   is_phase( isp ) = 0
	   is_cx   ( isp ) = hi_cx
	   is_uvar ( isp ) = is_uvar(isp - 1)
	   is_act  ( isp ) = isact_auxvar_regrid
	   is_sum  ( isp ) = 0    ! set to 1 only when returning after auxvars
	   RETURN 1
	ELSEIF (var_status .EQ. ferr_unknown_variable ) THEN
	   cx_stack_ptr = cx_stack_ptr - 1
	   GOTO 100
	ENDIF

* if the variable is a uvar set up a new isp level and report the dependencies
 900	   IF (.NOT.desist
     .	 .AND. cx_category(hi_cx) .EQ. cat_user_var) THEN
	   IF (its_gc) is_phase(isp) = is_phase(isp) - 1  ! WILL NEED WORK!!
* ... load the new variable info onto the IS stack
	   CALL PUSH_INTERP_STACK( isp, status )
	   IF ( status .NE. ferr_ok ) RETURN
	   is_cx   ( isp ) = hi_cx
	   is_uvar ( isp ) = cx_variable( hi_cx )
	   is_act  ( isp ) = isact_algebra
	   is_sum  ( isp ) = 0    ! flag that we are not returning from auxvars
	   RETURN 1
* if the variable is an aggregation from multiple datasets set up new isp
* level and report the ensemble members 
	ELSEIF  ( .NOT.desist
     .	    .AND. cx_category(hi_cx) .EQ. cat_aggregate_var ) THEN
	   CALL PUSH_INTERP_STACK( isp, status )
	   IF ( status .NE. ferr_ok ) RETURN
	   is_phase( isp ) = 0
	   is_cx   ( isp ) = hi_cx
	   is_uvar ( isp ) = is_uvar(isp - 1)
	   is_act  ( isp ) = isact_aggregate_gather
	   is_sum  ( isp ) = 0    ! set to 1 only when returning after auxvars
	   RETURN 1
* nothing more to do about the variable that was just reported
	ELSE
	   cx_stack_ptr = cx_stack_ptr - 1
	   GOTO 100
	ENDIF

* more aggregate variable support
!* grid for this aggregate dataset is not known.  Back to get_uvar_grid and call
!* is_aggregate_grid to get it
!* ... repeat this phase again when we get back
! 990	is_phase(isp) = is_phase(isp) - 1
!* load the new variable info onto the IS stack
!	CALL PUSH_INTERP_STACK( isp, status )
!	IF ( status .NE. ferr_ok ) RETURN
!c	is_sum( isp ) = isalg_merge_n
!
!	is_cx   ( isp ) = hi_cx
!	is_uvar ( isp ) = cx_variable( hi_cx )
!	is_act  ( isp ) = isact_get_agg_grid	! get aggregate grid on return.
!
!	RETURN 1
	
* clean up: release context and isp stack space
 1000	CONTINUE
	cx_stack_ptr = cx_stack_ptr - 1
	isp = isp - 1

! unclear -- is there special stuff to do for remote uvar definitions?
* Is it a LET/REMOTE variable?
* ! do we need to find a slot in rvar_uvar, and index the rvars with "rvar", 
* !or just use same indexing as for regular user-vars?
!        IF (uvar_remote(uvar)) THEN
!
!	   CALL CD_DSET_REMOTEOK(dset, ok_remote)
!	   IF (ok_remote) THEN
!	      uvar_remote(uvar) = .TRUE. 
!	      rvar_uvar(uvar) = uvar  
!	      rvar_dset(uvar) = dset
!c	      rvar_on_server(uvar) = .FALSE. 
!	   ELSE
!	      uvar_remote(uvar) = .FALSE. 
!	   ENDIF
!	ENDIF

	RETURN 2

* error exit
 5000	CALL DEALLO_ALL_GRIDS
	RETURN
! 9000	CALL ERRMSG( ferr_internal, status, 'is_uvar_grid', *5000 )

	END	