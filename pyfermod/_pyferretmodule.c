/*
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
 *  ARE DISCLAIMED. IN NO EVENT SHALL NOAA/PMEL/TMAP BE LIABLE FOR ANY
 *  SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 *  RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 *  CONTRACT, NEGLIGENCE OR OTHER TORTUOUS ACTION, ARISING OUT OF OR IN
 *  CONNECTION WITH THE ACCESS, USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <Python.h>
#define PY_ARRAY_UNIQUE_SYMBOL pyferret_ARRAY_API
#include <numpy/arrayobject.h>

#include "ferret.h"
#include "ferret_shared_buffer.h"
#include "EF_Util.h"
#include "pyferret.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Prototypes for some Ferret external function utility functions */
ExternalFunction *ef_ptr_from_id_ptr(int *id_ptr);
void ef_get_arg_subscripts_(int *id, int steplo[][4], int stephi[][4], int incr[][4]);
void ef_get_coordinates_(int *id, int *arg, int *axis, int *lo, int *hi, double *coords);
void ef_get_box_size_(int *id, int *arg, int *axis, int *lo, int *hi, float *sizes);
void ef_get_box_limits_(int *id, int *arg, int *axis, int *lo, int *hi, float *lo_lims, float *hi_lims);
void set_batch_graphics_(char *meta_name);

/* Ferret's OK return status value */
#define FERR_OK 3

/* Special return value from _pyferret._run indicating the program should shut down */
#define FERR_EXIT_PROGRAM -3

/* Ferret's unspecified integer value */
#define UNSPECIFIED_INT4 -999

/* Length given to the abstract axis */
#define ABSTRACT_AXIS_LEN 9999999

/* Flag of this Ferret's start/stop state */
static int ferretInitialized = 0;

/* for memory management in this module */
static size_t ferMemSize;
static float *ferMemory = NULL;
static float *pplMemory = NULL;

/* for recovering from seg faults */
static void (*segv_handler)(int);
static jmp_buf jumpbuffer;
static void pyefcn_signal_handler(int signum)
{
    longjmp(jumpbuffer, 1);
}


static char pyferretStartDocstring[] =
    "Initializes Ferret.  This allocates the initial amount of memory for Ferret \n"
    "(from Python-managed memory), opens the journal file, if requested, and sets \n"
    "Ferret's verify mode.  If metaname is empty, Ferret's graphics are displayed \n"
    "on the X-Windows display; otherwise, this value is used as the initial filename \n"
    "for the graphics metafile.  This routine does NOT run any user initialization \n"
    "scripts. \n"
    "\n"
    "Required arguments: \n"
    " (none) \n"
    "\n"
    "Optional arguments: \n"
    "    memsize = <float>: the size, in megafloats (where a floats is 4 bytes), \n"
    "                       to allocate for Ferret's memory cache (default 25.6) \n"
    "    journal = <bool>: initial state of Ferret's journal mode (default True) \n"
    "    verify = <bool>: initial state of Ferret's verify mode (default True) \n"
    "    metaname = <string>: filename for Ferret graphics (default empty) \n"
    "\n"
    "Returns: \n"
    "    True is successful \n"
    "    False if Ferret has already been started \n"
    "\n"
    "Raises: \n"
    "    MemoryError if unable to allocate the needed memory \n"
    "    IOError if unable to open the journal file \n";

static PyObject *pyferretStart(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *argNames[] = {"memsize", "journal", "verify", "metaname", NULL};
    double mwMemSize = 25.6;
    PyObject *pyoJournal = NULL;
    PyObject *pyoVerify = NULL;
    char *metaname = NULL;
    int journalFlag = 1;
    int verifyFlag = 1;
    int pplMemSize;
    size_t blksiz;
    int status;
    int ttoutLun = TTOUT_LUN;
    int one_cmnd_mode_int;

    /* If already initialized, return False */
    if ( ferretInitialized ) {
        Py_INCREF(Py_False);
        return Py_False;
    }

    /* Import the function-pointer table for the PyArray_* functions */
    import_array1(NULL);

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "|dO!O!s", argNames, &mwMemSize,
                 &PyBool_Type, &pyoJournal, &PyBool_Type, &pyoVerify, &metaname) )
        return NULL;

    /* Interpret the booleans - Py_False and Py_True are singleton non-NULL objects, so just use == */
    if ( pyoJournal == Py_False )
        journalFlag = 0;
    if ( pyoVerify == Py_False )
        verifyFlag = 0;
    if ( metaname[0] == '\0' )
        metaname = NULL;

    /* Initialize the shared buffer sBuffer */
    set_shared_buffer();

    /* Initial allocation of PPLUS memory */
    pplMemSize = 0.5 * 1.0E6;
    pplMemory = (float *) PyMem_Malloc((size_t)pplMemSize * (size_t)sizeof(float));
    if ( pplMemory == NULL )
        return PyErr_NoMemory();
    set_ppl_memory(pplMemory, pplMemSize);

    /* Initial allocation of Ferret memory - multiples of 100 PMAX_MEM_BLKS */
    blksiz  = (size_t) ((mwMemSize * 1.0E6 + (double)PMAX_MEM_BLKS - 1.0) / (double)PMAX_MEM_BLKS);
    blksiz  = (blksiz + 99) / 100;
    blksiz *= 100;
    ferMemSize = blksiz * (size_t)PMAX_MEM_BLKS;
    /* Check for overflow */
    if ( blksiz != ferMemSize / (size_t)PMAX_MEM_BLKS )
        return PyErr_NoMemory();
    ferMemory = (float *) PyMem_Malloc(ferMemSize * (size_t)sizeof(float));
    if ( ferMemory == NULL )
        return PyErr_NoMemory();
    set_fer_memory(ferMemory, ferMemSize);

    /* Inhibit graphics display if requested */
    if ( metaname != NULL ) {
       /* Make a copy of the name just in case set_batch_graphics_ changes something */
       char my_meta_name[256];
       strncpy(my_meta_name, metaname, 256);
       my_meta_name[255] = '\0';
       set_batch_graphics_(my_meta_name);
    }

    /* Initialize stuff: keyboard, todays date, grids, GFDL terms, PPL brain */
    initialize_();

    /* Open the output journal file, if appropriate */
    if ( journalFlag != 0 ) {
        init_journal_(&status);
        if ( status != FERR_OK ) {
            PyErr_SetString(PyExc_IOError, "Unable to open the journal file ferret.jnl");
            return NULL;
        }
    }
    else
        no_journal_();

    /* Set the verify flag */
    if ( verifyFlag == 0 )
        turnoff_verify_(&status);

    /* Output Program name and revision number */
    proclaim_c_(&ttoutLun, "\t");

    /* Set so that ferret_dispatch returns after every command */
    one_cmnd_mode_int = 1;
    set_one_cmnd_mode_(&one_cmnd_mode_int);

    /* Success - return True */
    ferretInitialized = 1;
    Py_INCREF(Py_True);
    return Py_True;
}


/*
 * Helper function to reallocate Ferret's memory from Python.
 * Argument: the new number of floats of Ferret's memory cache is given by
 *           blksiz * PMAX_MEM_BLKS (defined in ferret.h as 2000)
 * Returns: zero if fails, non-zero if successful
 */
static int resizeFerretMemory(int blksiz)
{
    size_t actual_blksiz;
    size_t newFerMemSize;

    /* Get the new size for the memory and check for overflow */
    if ( blksiz <= 0 )
        return 0;
    actual_blksiz = (blksiz  + 99)/ 100;
    actual_blksiz *= 100;
    newFerMemSize = actual_blksiz * (size_t)PMAX_MEM_BLKS;
    if ( actual_blksiz != newFerMemSize / (size_t)PMAX_MEM_BLKS )
        return 0;

    /*
     * Free the old memory and allocate new memory rather than use
     * realloc since the contents of the old memory isn't needed.
     * This could also result in a better garbage collection.
     */
    PyMem_Free(ferMemory);
    ferMemory = (float *) PyMem_Malloc(newFerMemSize * (size_t)sizeof(float));
    if ( ferMemory == NULL ) {
        ferMemory = (float *) PyMem_Malloc(ferMemSize * (size_t)sizeof(float));
        if ( ferMemory == NULL ) {
            fprintf(stderr, "**ERROR: Unable to restore Ferret's memory cache of %f Mfloats\n", (double)ferMemSize / 1.0E6);
            exit(1);
        }
        return 0;
    }

    /* Reallocation successful; assign the new memory */
    ferMemSize = newFerMemSize;
    set_fer_memory(ferMemory, ferMemSize);
    return 1;
}


static char pyferretResizeMemoryDocstring[] =
    "Reset the the amount of memory allocated for Ferret from Python-managed memory. \n"
    "\n"
    "Required arguments: \n"
    "    memsize = <float>: the new size, in megafloats (where a float is 4 bytes), \n"
    "                       for Ferret's memory cache \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    True if successful - Ferret has the new amount of memory \n"
    "    False if unsuccessful - Ferret has the previous amount of memory \n"
    "\n"
    "Raises: \n"
    "    MemoryError if Ferret has not been started or has been stopped \n";

static PyObject *pyferretResizeMemory(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *argNames[] = {"memsize", NULL};
    double mwMemSize;

    /* If not initialized, raise a MemoryError */
    if ( ! ferretInitialized ) {
        PyErr_SetString(PyExc_MemoryError, "Ferret not started");
        return NULL;
    }

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "d", argNames, &mwMemSize) )
        return NULL;

    /* Reallocate the new amount of memory for Ferret */
    if ( resizeFerretMemory((int) ((mwMemSize * 1.0E6 + (double)PMAX_MEM_BLKS - 1.0) / (double)PMAX_MEM_BLKS)) == 0 ) {
        Py_INCREF(Py_False);
        return Py_False;
    }

    /* Success - return True */
    Py_INCREF(Py_True);
    return Py_True;
}


static char pyferretRunCommandDocstring[] =
    "Runs a Ferret command just as if entering a command at the Ferret prompt. \n"
    "\n"
    "If an empty string is given, Ferret will prompt you for commands \n"
    "until either the \"EXIT\" or the \"EXIT /TOPYTHON\" command is given. \n"
    "If \"EXIT /TOPYTHON\" is given, the return tuple will be for the last \n"
    "error, if any, that occurred in the sequence of commands submitted. \n"
    "\n"
    "Required arguments: \n"
    "    command = <string>: the Ferret command to be run \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "   (err_int, err_string) \n"
    "       err_int: one of the FERR_* data values (FERR_OK if there are no errors) \n"
    "       err_string: error or warning message (can be empty) \n"
    "   Error messages normally start with \"**ERROR\" \n"
    "   Warning messages normally start with \"*** NOTE:\" \n"
    "\n"
    "Raises: \n"
    "    MemoryError if Ferret has not been started or has been stopped \n";

static PyObject *pyferretRunCommand(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *argNames[] = {"command", NULL};
    const char *command;
    const char *iter_command;
    int  one_cmnd_mode_int;
    int  cmnd_stack_level;
    char errmsg[2112];
    int  errval;

    /* If not initialized, raise a MemoryError */
    if ( ! ferretInitialized ) {
        PyErr_SetString(PyExc_MemoryError, "Ferret not started");
        return NULL;
    }

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "s", argNames, &command) )
        return NULL;

    /* Clear the last error message and value */
    clear_fer_last_error_info_();

    /* If an empty string, temporarily turn off the one-command mode */
    if ( command[0] == '\0' ) {
        one_cmnd_mode_int = 0;
        set_one_cmnd_mode_(&one_cmnd_mode_int);
    }
    else
        one_cmnd_mode_int = 1;

    /* do-loop only for dealing with Ferret "SET MEMORY /SIZE=..." resize command */
    iter_command = command;
    do {
        cmnd_stack_level = 0;
        /* Run the Ferret command */
        ferret_dispatch_c(ferMemory, iter_command, sBuffer);

        if ( sBuffer->flags[FRTN_ACTION] == FACTN_MEM_RECONFIGURE ) {
            /* resize, then re-enter if not single-command mode */
            if ( resizeFerretMemory(sBuffer->flags[FRTN_IDATA1]) == 0 ) {
                printf("Unable to resize Ferret's memory cache to %f Mfloats\n",
                       (double)(sBuffer->flags[FRTN_IDATA1]) * (double)PMAX_MEM_BLKS / 1.0E6);
                printf("Ferret's memory cache remains at %f Mfloats\n",
                       (double)(ferMemSize) / 1.0E6);
            }
            cmnd_stack_level = sBuffer->flags[FRTN_IDATA2];
            /* submit an empty command after resizing to continue on with whaterever was going on */
            iter_command = "";
        }
        else {
            /* not a memory resize command (probably an exit command) */
            break;
        }
    } while ( (one_cmnd_mode_int == 0) || (cmnd_stack_level > 0) );

    /* Set back to single command mode */
    if ( one_cmnd_mode_int == 0 ) {
        one_cmnd_mode_int = 1;
        set_one_cmnd_mode_(&one_cmnd_mode_int);
    }

    if ( sBuffer->flags[FRTN_ACTION] == FACTN_EXIT ) {
        /* plain "EXIT" Ferret command - instigate an orderly shutdown */

        /* return Py_BuildValue("is", FERR_EXIT_PROGRAM, "EXIT"); */

        /*
         * python -i -c ... intercepts the Python sys.exit() call and stays in python,
         * so just do a C exit() from python
         */
        exit(0);
    }

    /* Get the last error message (null terminated) and value */
    get_fer_last_error_info_(&errval, errmsg, 2112);

    /* Return the tuple of the last error value and message */
    return Py_BuildValue("is", errval, errmsg);
}


static char pyferretGetDataDocstring[] =
    "Returns the numeric data array described in the argument. \n"
    "\n"
    "Required arguments: \n"
    "    name = <string>: the name of the numeric data array to return \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    A tuple containing: \n"
    "        a NumPy float32 ndarray containing a copy of the numeric data requested, \n"
    "        a NumPy float32 ndarray containing the bad-data-flag value(s) for the data, \n"
    "        a string giving the units for the data \n"
    "        a tuple of four integers giving the AXISTYPE codes of the axes, \n"
    "        a tuple of four strings giving the names of the axes, \n"
    "        a tuple of four strings giving the units of a non-calendar-time data axis, or \n"
    "                                       the CALTYPE_ calendar name of a calendar-time axis, \n"
    "        a tuple of four ndarrays giving the coordinates for the data axes \n"
    "            (ndarray of N doubles for non-calendar-time, non-normal axes, \n"
    "             ndarray of (N,6) integers for calendar-time axes, or \n"
    "             None for normal axes) \n"
    "\n"
    "Raises: \n"
    "    ValueError if the data name is invalid \n"
    "    MemoryError if Ferret has not been started or has been stopped \n";

static PyObject *pyferretGetData(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *argNames[] = {"name", NULL};
    char        *name;
    int          lendataname;
    char         dataname[1024];
    int          arraystart;
    int          memlo[MAX_FERRET_NDIM], memhi[MAX_FERRET_NDIM];
    int          steplo[MAX_FERRET_NDIM], stephi[MAX_FERRET_NDIM], incr[MAX_FERRET_NDIM];
    char         dataunit[64];
    int          lendataunit;
    AXISTYPE     axis_types[MAX_FERRET_NDIM];
    char         errmsg[2112];
    int          lenerrmsg;
    float        badval;
    int          i, j, k, l, q;
    npy_intp     shape[MAX_FERRET_NDIM];
    npy_intp     new_shape[2];
    int          strides[MAX_FERRET_NDIM];
    PyObject    *data_ndarray;
    float       *ferdata;
    float       *npydata;
    PyObject    *badval_ndarray;
    PyObject    *axis_coords[MAX_FERRET_NDIM];
    char         axis_units[MAX_FERRET_NDIM][64];
    char         axis_names[MAX_FERRET_NDIM][64];
    CALTYPE      calendar_type;

    /* If not initialized, raise a MemoryError */
    if ( ! ferretInitialized ) {
        PyErr_SetString(PyExc_MemoryError, "Ferret not started");
        return NULL;
    }

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "s", argNames, &name) )
        return NULL;

    /* Make a copy of dataname just to be sure it isn't altered */
    lendataname = strlen(name);
    if ( lendataname > 1020 ) {
        PyErr_SetString(PyExc_ValueError, "name too long");
        return NULL;
    }
    strcpy(dataname, name);

    /*
     * Retrieve the memory parameters describing the data array requested.
     * Assumes Unix standard for passing strings to Fortran (appended array lengths).
     */
    get_data_array_params_(dataname, &lendataname, ferMemory, &arraystart, memlo, memhi,
                           steplo, stephi, incr, dataunit, &lendataunit, axis_types,
                           &badval, errmsg, &lenerrmsg, 1024, 64, 2112);
    if ( lenerrmsg > 0 ) {
        errmsg[lenerrmsg] = '\0';
        PyErr_SetString(PyExc_ValueError, errmsg);
        return NULL;
    }

    /* null terminate the data unit name */
    dataunit[lendataunit] = '\0';

    /* Get the shape of the array */
    for (k = 0; k < MAX_FERRET_NDIM; k++)
        shape[k] = (npy_intp) ((stephi[k] - steplo[k] + incr[k]) / (incr[k]));

    /* Get the strides through the memory (as a float *) */
    strides[0] = 1;
    for (k = 1; k < MAX_FERRET_NDIM; k++)
        strides[k] = strides[k-1] * (memhi[k-1] - memlo[k-1] + 1);

    /* Get the actual starting point in the array */
    for (k = 0; k < MAX_FERRET_NDIM; k++)
        arraystart += (strides[k]) * (steplo[k] - memlo[k]);

    /* Convert to strides through places in memory to be read */
    for (k = 0; k < MAX_FERRET_NDIM; k++)
        strides[k] *= incr[k];

    /* Create a new NumPy float ndarray (Fortran ordering) with the same shape */
    data_ndarray = PyArray_EMPTY(MAX_FERRET_NDIM, shape, NPY_FLOAT, 1);
    if ( data_ndarray == NULL ) {
        return NULL;
    }

    /*
     * Assign the data in the new ndarray.
     * Note: if MAX_FERRET_NDIM changes, this needs editing.
     */
    ferdata = ferMemory + arraystart;
    npydata = PyArray_DATA(data_ndarray);
    q = 0;
    for (l = 0; l < (int)(shape[3]); l++) {
        for (k = 0; k < (int)(shape[2]); k++) {
            for (j = 0; j < (int)(shape[1]); j++) {
                for (i = 0; i < (int)(shape[0]); i++) {
                   npydata[q] = ferdata[i * strides[0] + j * strides[1] + k * strides[2] + l * strides[3]];
                   q++;
                }
            }
        }
    }

    /* Create a new NumPy float ndarray with the bad-data-flag value(s) */
    new_shape[0] = 1;
    badval_ndarray = PyArray_SimpleNew(1, new_shape, NPY_FLOAT);
    if ( badval_ndarray == NULL ) {
       Py_DECREF(data_ndarray);
       return NULL;
    }
    npydata = PyArray_DATA(badval_ndarray);
    npydata[0] = badval;

    /* Create the axis coordinates array objects */
    for (k = 0; k < MAX_FERRET_NDIM; k++) {
        switch( axis_types[k] ) {
        case AXISTYPE_LONGITUDE:
        case AXISTYPE_LATITUDE:
        case AXISTYPE_LEVEL:
        case AXISTYPE_CUSTOM:
        case AXISTYPE_ABSTRACT:
            /* array of doubles, possibly with a units string */
            axis_coords[k] = PyArray_SimpleNew(1, &(shape[k]), NPY_DOUBLE);
            if ( axis_coords[k] == NULL ) {
                while ( k > 0 ) {
                    k--;
                    Py_DECREF(axis_coords[k]);
                }
                Py_DECREF(badval_ndarray);
                Py_DECREF(data_ndarray);
                return NULL;
            }
            /* get the coordinates and the units string */
            q = k+1;
            j = (int) shape[k];
            get_data_array_coords_((double *)PyArray_DATA(axis_coords[k]), axis_units[k],
                                   axis_names[k], &q, &j, errmsg, &lenerrmsg, 64, 64, 2112);
            if ( lenerrmsg > 0 ) {
                errmsg[lenerrmsg] = '\0';
                PyErr_SetString(PyExc_ValueError, errmsg);
                Py_DECREF(axis_coords[k]);
                while ( k > 0 ) {
                    k--;
                    Py_DECREF(axis_coords[k]);
                }
                Py_DECREF(badval_ndarray);
                Py_DECREF(data_ndarray);
                return NULL;
            }
            break;
        case AXISTYPE_TIME:
            /* array of 6-tuples of integers in C order, so: [N][6] in C or (6,N) in Fortran */
            new_shape[0] = shape[k];
            new_shape[1] = 6;
            axis_coords[k] = PyArray_SimpleNew(2, new_shape, NPY_INT);
            if ( axis_coords[k] == NULL ) {
                while ( k > 0 ) {
                    k--;
                    Py_DECREF(axis_coords[k]);
                }
                Py_DECREF(badval_ndarray);
                Py_DECREF(data_ndarray);
                return NULL;
            }
            /* get the time coordinate integers */
            q = k+1;
            j = (int) shape[k];
            get_data_array_time_coords_((int (*)[6])PyArray_DATA(axis_coords[k]), &calendar_type, axis_names[k],
                                        &q, &j, errmsg, &lenerrmsg, 64, 2112);
            if ( lenerrmsg > 0 ) {
                errmsg[lenerrmsg] = '\0';
                PyErr_SetString(PyExc_ValueError, errmsg);
                Py_DECREF(axis_coords[k]);
                while ( k > 0 ) {
                    k--;
                    Py_DECREF(axis_coords[k]);
                }
                Py_DECREF(badval_ndarray);
                Py_DECREF(data_ndarray);
                return NULL;
            }
            /* set the axis units to the name of the calendar */
            switch( calendar_type ) {
            case CALTYPE_NONE:
                strcpy(axis_units[k], "CALTYPE_NONE");
                break;
            case CALTYPE_360DAY:
                strcpy(axis_units[k], "CALTYPE_360DAY");
                break;
            case CALTYPE_NOLEAP:
                strcpy(axis_units[k], "CALTYPE_NOLEAP");
                break;
            case CALTYPE_GREGORIAN:
                strcpy(axis_units[k], "CALTYPE_GREGORIAN");
                break;
            case CALTYPE_JULIAN:
                strcpy(axis_units[k], "CALTYPE_JULIAN");
                break;
            case CALTYPE_ALLLEAP:
                strcpy(axis_units[k], "CALTYPE_ALLLEAP");
                break;
            default:
                sprintf(errmsg, "Unexpected calendar type of %d", calendar_type);
                PyErr_SetString(PyExc_ValueError, errmsg);
                Py_DECREF(axis_coords[k]);
                while ( k > 0 ) {
                    k--;
                    Py_DECREF(axis_coords[k]);
                }
                Py_DECREF(badval_ndarray);
                Py_DECREF(data_ndarray);
                return NULL;
            }
            break;
        case AXISTYPE_NORMAL:
            /* axis normal to the results - no coordinates */
            Py_INCREF(Py_None);
            axis_coords[k] = Py_None;
            axis_units[k][0] = '\0';
            axis_names[k][0] = '\0';
            break;
        default:
            sprintf(errmsg, "Unexpected axis type of %d", axis_types[k]);
            PyErr_SetString(PyExc_RuntimeError, errmsg);
            while ( k > 0 ) {
                k--;
                Py_DECREF(axis_coords[k]);
            }
            Py_DECREF(badval_ndarray);
            Py_DECREF(data_ndarray);
            return NULL;
        }
    }

    /*
     * Return a tuple (stealing references for PyObjects) of data_ndarray,
     * badval_ndarray, dataunit, axis_types, axis_names, axis_units, and axis_coords.
     * Note: if MAX_FERRET_NDIM changes, this needs editing.
     */
    return Py_BuildValue("NNs(iiii)(ssss)(ssss)(NNNN)", data_ndarray, badval_ndarray, dataunit,
                         axis_types[0], axis_types[1], axis_types[2], axis_types[3],
                         axis_names[0], axis_names[1], axis_names[2], axis_names[3],
                         axis_units[0], axis_units[1], axis_units[2], axis_units[3],
                         axis_coords[0], axis_coords[1], axis_coords[2], axis_coords[3]);
}


static char pyferretPutDataDocstring[] =
    "Creates a Ferret data variable with the numeric data array described in the arguments. \n"
    "\n"
    "Required arguments: \n"
    "    codename = <string>: the code name of the Ferret data variable to create (eg, \"SST\") \n"
    "    title = <string>: the title of the Ferret data variable to create (eg, \"Sea Surface Temperature\") \n"
    "    data = <ndarray>: the array containing the numeric data \n"
    "    bdfval = <ndarray>: the bad-data-flag value for the data \n"
    "    units = <string>: the units for the data \n"
    "    dset = <string>: the dataset name or number to be associates with this variable; \n"
    "                     give an empty strip associated with the current dataset \n"
    "                     or 'None' to not associate with any dataset \n"
    "    axis_types = <4-tuple of int>: the AXISTYPE codes for the axes \n"
    "    axis_names = <4-tuple of string>: the names of the axes \n"
    "    axis_units = <4-tuple of string>: the units of a non-calendar-time axis, or \n"
    "                                      the CALTYPE_ calendar name of a calendar-time axis \n"
    "    axis_coords = <4-tuple of ndarray>: the axis coordinates \n"
    "                                        (ndarray of N doubles for a non-calendar-time, non-normal axis, or \n"
    "                                         ndarray of (N,6) integers for a calendar-time axis; \n"
    "                                         None - or any object - for a normal axis) \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    None \n"
    "\n"
    "Raises: \n"
    "    ValueError if there is a problem with the argument data passed \n"
    "    MemoryError if Ferret has not been started or has been stopped \n";

static PyObject *pyferretPutData(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *argNames[] = {"codename", "title", "data", "bdfval", "units", "dset",
                               "axis_types", "axis_names", "axis_units", "axis_coords", NULL};
    char        *codename;
    char        *title;
    PyObject    *data_ndarray;
    PyObject    *bdfval_ndarray;
    char        *units;
    char        *dset;
    PyObject    *axis_types_tuple;
    PyObject    *axis_names_tuple;
    PyObject    *axis_units_tuple;
    PyObject    *axis_coords_tuple;
    float        bdfval;
    int          k;
    PyObject    *seqitem;
    AXISTYPE     axis_types[MAX_FERRET_NDIM];
    char        *strptr;
    char         axis_names[MAX_FERRET_NDIM][64];
    char         axis_units[MAX_FERRET_NDIM][64];
    int          num_coords[MAX_FERRET_NDIM];
    void        *axis_coords[MAX_FERRET_NDIM];
    CALTYPE      calendar_type;
    int          axis_nums[MAX_FERRET_NDIM];
    int          axis_starts[MAX_FERRET_NDIM];
    int          axis_ends[MAX_FERRET_NDIM];
    int          len_codename;
    int          len_title;
    int          len_units;
    int          len_dset;
    char         errmsg[2048];
    int          len_errmsg;

    /* If not initialized, raise a MemoryError */
    if ( ! ferretInitialized ) {
        PyErr_SetString(PyExc_MemoryError, "Ferret not started");
        return NULL;
    }

    /* Parse the arguments, checking if an Exception was raised - borrowed references to the PyObjects */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "ssOOssOOOO", argNames, &codename, &title,
                                       &data_ndarray, &bdfval_ndarray, &units, &dset, &axis_types_tuple,
                                       &axis_names_tuple, &axis_units_tuple, &axis_coords_tuple) )
        return NULL;

    /* PyArray_Size returns 0 if the object is not an appropriate type */
    /* ISFARRAY_RO checks if it is F-contiguous, aligned, and in machine byte-order */
    if ( (PyArray_Size(data_ndarray) < 1) || (PyArray_TYPE(data_ndarray) != NPY_FLOAT) ||
         (! PyArray_ISFARRAY_RO(data_ndarray)) || (! PyArray_CHKFLAGS(data_ndarray, NPY_OWNDATA)) ) {
        PyErr_SetString(PyExc_ValueError, "data is not an appropriate ndarray of type float32");
        return NULL;
    }


    /* PyArray_Size returns 0 if the object is not an appropriate type */
    /* ISBEHAVED_RO checks if it is aligned and in machine byte-order */
    if ( (PyArray_Size(bdfval_ndarray) < 1) || (PyArray_TYPE(bdfval_ndarray) != NPY_FLOAT) ||
         (! PyArray_ISBEHAVED_RO(bdfval_ndarray)) ) {
        PyErr_SetString(PyExc_ValueError, "bdfval is not an appropriate ndarray of type float32");
        return NULL;
    }
    /* Just get bdfval from the data in bdfval_ndarray */
    bdfval = ((float *) PyArray_DATA(bdfval_ndarray))[0];

    /* Get the axis types out of the tuple */
    axis_types_tuple = PySequence_Fast(axis_types_tuple, "axis_types is not a tuple or list");
    if ( axis_types_tuple == NULL ) {
        return NULL;
    }
    if ( PySequence_Fast_GET_SIZE(axis_types_tuple) != MAX_FERRET_NDIM ) {
        PyErr_SetString(PyExc_ValueError, "axis_types does not have the expected number of items");
        Py_DECREF(axis_types_tuple);
        return NULL;
    }
    for (k = 0; k < MAX_FERRET_NDIM; k++) {
        seqitem = PySequence_Fast_GET_ITEM(axis_types_tuple, k); /* borrowed reference */
        axis_types[k] = (int) PyInt_AsLong(seqitem);
        if ( (axis_types[k] != AXISTYPE_LONGITUDE) &&
             (axis_types[k] != AXISTYPE_LATITUDE) &&
             (axis_types[k] != AXISTYPE_LEVEL) &&
             (axis_types[k] != AXISTYPE_TIME) &&
             (axis_types[k] != AXISTYPE_CUSTOM) &&
             (axis_types[k] != AXISTYPE_ABSTRACT) &&
             (axis_types[k] != AXISTYPE_NORMAL) ) {
            PyErr_SetString(PyExc_ValueError, "Invalid axis_types item");
            Py_DECREF(axis_types_tuple);
            return NULL;
        }
    }
    Py_DECREF(axis_types_tuple);

    /* Get the axis names out of the tuple */
    axis_names_tuple = PySequence_Fast(axis_names_tuple, "axis_names is not a tuple or list");
    if ( axis_names_tuple == NULL ) {
        return NULL;
    }
    if ( PySequence_Fast_GET_SIZE(axis_names_tuple) != MAX_FERRET_NDIM ) {
        PyErr_SetString(PyExc_ValueError, "axis_names does not have the expected number of items");
        Py_DECREF(axis_names_tuple);
        return NULL;
    }
    for (k = 0; k < MAX_FERRET_NDIM; k++) {
        seqitem = PySequence_Fast_GET_ITEM(axis_names_tuple, k); /* borrowed reference */
        strptr = PyString_AsString(seqitem);
        if ( strptr == NULL ) {
            PyErr_Clear();
            PyErr_SetString(PyExc_ValueError, "Invalid axis_names item");
            Py_DECREF(axis_names_tuple);
            return NULL;
        }
        strncpy(axis_names[k], strptr, 64);
        axis_names[k][63] = '\0';
    }
    Py_DECREF(axis_names_tuple);

    /* Get the axis units out of the tuple */
    axis_units_tuple = PySequence_Fast(axis_units_tuple, "axis_units is not a tuple or list");
    if ( axis_units_tuple == NULL ) {
        return NULL;
    }
    if ( PySequence_Fast_GET_SIZE(axis_units_tuple) != MAX_FERRET_NDIM ) {
        PyErr_SetString(PyExc_ValueError, "axis_units does not have the expected number of items");
        Py_DECREF(axis_units_tuple);
        return NULL;
    }
    for (k = 0; k < MAX_FERRET_NDIM; k++) {
        seqitem = PySequence_Fast_GET_ITEM(axis_units_tuple, k); /* borrowed reference */
        strptr = PyString_AsString(seqitem);
        if ( strptr == NULL ) {
            PyErr_Clear();
            PyErr_SetString(PyExc_ValueError, "Invalid axis_units item");
            Py_DECREF(axis_units_tuple);
            return NULL;
        }
        strncpy(axis_units[k], strptr, 64);
        axis_units[k][63] = '\0';
    }
    Py_DECREF(axis_units_tuple);

    /* Get the axis coordinates ndarray out of the tuple */
    axis_coords_tuple = PySequence_Fast(axis_coords_tuple, "axis_coords is not a tuple or list");
    if ( axis_coords_tuple == NULL ) {
        return NULL;
    }
    if ( PySequence_Fast_GET_SIZE(axis_coords_tuple) != MAX_FERRET_NDIM ) {
        PyErr_SetString(PyExc_ValueError, "axis_coords does not have the expected number of items");
        Py_DECREF(axis_coords_tuple);
        return NULL;
    }
    for (k = 0; k < MAX_FERRET_NDIM; k++) {
        seqitem = PySequence_Fast_GET_ITEM(axis_coords_tuple, k); /* borrowed reference */
        switch( axis_types[k] ) {
        case AXISTYPE_LONGITUDE:
        case AXISTYPE_LATITUDE:
        case AXISTYPE_LEVEL:
        case AXISTYPE_CUSTOM:
        case AXISTYPE_ABSTRACT:
            /* float64 N-ndarray containing the axis coordinates */
            /* PyArray_Size returns 0 if the object is not an appropriate type */
            /* ISCARRAY_RO checks if it is C-contiguous, aligned and in machine byte-order */
            num_coords[k] = PyArray_Size(seqitem);
            if ( (num_coords[k] < 1) || (PyArray_TYPE(seqitem) != NPY_DOUBLE) ||
                 (! PyArray_ISCARRAY_RO(seqitem)) ) {
                PyErr_SetString(PyExc_ValueError, "an item of axis_coords is not an appropriate ndarray of type float64");
                Py_DECREF(axis_coords_tuple);
                return NULL;
            }
            axis_coords[k] = PyArray_DATA(seqitem);
            get_axis_num_(&(axis_nums[k]), &(axis_starts[k]), &(axis_ends[k]), axis_names[k],
                          axis_units[k], axis_coords[k], &(num_coords[k]), &(axis_types[k]),
                          errmsg, &len_errmsg, strlen(axis_names[k]), strlen(axis_units[k]), 2048);
            if ( len_errmsg > 0 ) {
                errmsg[len_errmsg] = '\0';
                PyErr_SetString(PyExc_ValueError, errmsg);
                Py_DECREF(axis_coords_tuple);
                return NULL;
            }
            break;
        case AXISTYPE_TIME:
            /* int32 (N,6)-ndarray containing component time values; the calendar given in axis_units */
            /* PyArray_Size returns 0 if the object is not an appropriate type */
            /* ISCARRAY_RO checks if it is C-contiguous, aligned and in machine byte-order */
            num_coords[k] = PyArray_Size(seqitem);
            if ( (num_coords[k] < 1) || ((num_coords[k] % 6) != 0) || (PyArray_TYPE(seqitem) != NPY_INT) ||
                 (! PyArray_ISCARRAY_RO(seqitem)) ) {
                PyErr_SetString(PyExc_ValueError, "an item of axis_coords is not an appropriate ndarray of type int32");
                Py_DECREF(axis_coords_tuple);
                return NULL;
            }
            num_coords[k] /= 6;
            if ( strcmp(axis_units[k], "CALTYPE_NONE") == 0 ) {
                calendar_type = CALTYPE_NONE;
            }
            else if ( strcmp(axis_units[k], "CALTYPE_360DAY") == 0 ) {
                calendar_type = CALTYPE_360DAY;
            }
            else if ( strcmp(axis_units[k], "CALTYPE_NOLEAP") == 0 ) {
                calendar_type = CALTYPE_NOLEAP;
            }
            else if ( strcmp(axis_units[k], "CALTYPE_GREGORIAN") == 0 ) {
                calendar_type = CALTYPE_GREGORIAN;
            }
            else if ( strcmp(axis_units[k], "CALTYPE_JULIAN") == 0 ) {
                calendar_type = CALTYPE_JULIAN;
            }
            else if ( strcmp(axis_units[k], "CALTYPE_ALLLEAP") == 0 ) {
                calendar_type = CALTYPE_ALLLEAP;
            }
            else {
                PyErr_SetString(PyExc_ValueError, "unknown calendar");
                Py_DECREF(axis_coords_tuple);
                return NULL;
            }
            axis_coords[k] = PyArray_DATA(seqitem);
            get_time_axis_num_(&(axis_nums[k]), &(axis_starts[k]), &(axis_ends[k]),
                               axis_names[k], &calendar_type, axis_coords[k], &(num_coords[k]),
                               errmsg, &len_errmsg, strlen(axis_names[k]), 2048);
            if ( len_errmsg > 0 ) {
                errmsg[len_errmsg] = '\0';
                PyErr_SetString(PyExc_ValueError, errmsg);
                Py_DECREF(axis_coords_tuple);
                return NULL;
            }
            break;
        case AXISTYPE_NORMAL:
            /* axis normal to the results - ignore sequence item (probably None) */
            axis_nums[k] = 0;   /* ferret.parm value for a normal line (mnormal) */
            axis_starts[k] = 0;
            axis_ends[k] = 0;
            break;
        default:
            PyErr_SetString(PyExc_ValueError, "Unexpected axis_type when processing axis coordinates");
            Py_DECREF(axis_coords_tuple);
            return NULL;
        }
    }

    /* The information in axis_coords_tuple no longer needed */
    Py_DECREF(axis_coords_tuple);

    /* Assign the data in the XPYVAR_INFO common block */
    len_codename = strlen(codename);
    len_title = strlen(title);
    len_units = strlen(units);
    len_dset = strlen(dset);
    add_pystat_var_(&data_ndarray, codename, title, units, &bdfval, dset,
                    axis_nums, axis_starts, axis_ends, errmsg, &len_errmsg,
                    len_codename, len_title, len_units, len_dset, 2048);
    if ( len_errmsg > 0 ) {
        errmsg[len_errmsg] = '\0';
        PyErr_SetString(PyExc_ValueError, errmsg);
        return NULL;
    }

    /*
     * Increase the reference count to data_ndarray to keep it around.
     * A pointer to it is stored in the XPYVAR_INFO common block.
     * The reference count will be decremented by Ferret when no longer needed.
     */
    Py_INCREF(data_ndarray);

    Py_INCREF(Py_None);
    return Py_None;
}

static char pyferretStopDocstring[] =
    "Shuts down and release all memory used by Ferret. \n"
    "After calling this function do not call any Ferret functions except start, \n"
    "which will restart Ferret and re-enable the other functions. \n"
    "\n"
    "Required arguments: \n"
    "    (none) \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    False if Ferret has not been started or has already been stopped \n"
    "    True otherwise \n";

static PyObject *pyferretStop(PyObject *self)
{
    /* If not initialized, return False */
    if ( ! ferretInitialized ) {
        Py_INCREF(Py_False);
        return Py_False;
    }

    /* Set to uninitialized */
    ferretInitialized = 0;

    /* Run commands to clear/reset Ferret's state */
    ferret_dispatch_c(ferMemory, "SET GRID ABSTRACT", sBuffer);
    ferret_dispatch_c(ferMemory, "CANCEL VARIABLE/ALL", sBuffer);
    ferret_dispatch_c(ferMemory, "CANCEL SYMBOL/ALL", sBuffer);
    ferret_dispatch_c(ferMemory, "CANCEL DATA/ALL", sBuffer);
    ferret_dispatch_c(ferMemory, "CANCEL REGION/ALL", sBuffer);
    ferret_dispatch_c(ferMemory, "CANCEL MEMORY/ALL", sBuffer);
    ferret_dispatch_c(ferMemory, "EXIT", sBuffer);

    /* Free memory allocated inside Ferret */
    finalize_();

    /* Free memory allocated for Ferret */
    PyMem_Free(ferMemory);
    ferMemory = NULL;
    ferMemSize = 0;
    PyMem_Free(pplMemory);
    pplMemory = NULL;

    /* Return True */
    Py_INCREF(Py_True);
    return Py_True;
}


static char pyefcnGetAxisCoordinatesDocstring[] =
    "Returns the \"world\" coordinates for an axis of an argument to an external function\n"
    "\n"
    "Required arguments: \n"
    "    id = <int>: the ferret id of the external function \n"
    "    arg = <int>: the index (zero based) of the argument (can use ARG1, ARG2, ..., ARG9) \n"
    "    axis = <int>: the index (zero based) of the axis (can use X_AXIS, Y_AXIS, Z_AXIS, T_AXIS) \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    a NumPy float64 ndarray containing the \"world\" coordinates, \n"
    "    or None if the values cannot be determined at the time this was called \n"
    "\n"
    "Raises: \n"
    "    ValueError if id, arg, or axis is invalid \n";

static PyObject *pyefcnGetAxisCoordinates(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char      *argNames[] = {"id", "arg", "axis", NULL};
    int               id, arg, axis;
    ExternalFunction *ef_ptr;
    int               steplo[EF_MAX_COMPUTE_ARGS][4], stephi[EF_MAX_COMPUTE_ARGS][4], incr[EF_MAX_COMPUTE_ARGS][4];
    int               lo, hi;
    npy_intp          shape[1];
    PyObject         *coords_ndarray;

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "iii", argNames, &id, &arg, &axis) )
        return NULL;

    /* Check for obvious errors in the arguments passed */
    ef_ptr = ef_ptr_from_id_ptr(&id);
    if ( (ef_ptr == NULL) || ! ef_ptr->already_have_internals ) {
        PyErr_SetString(PyExc_ValueError, "Invalid ferret external function id");
        return NULL;
    }
    if ( (arg < 0) || (arg >= EF_MAX_ARGS) ||
         ((arg >= ef_ptr->internals_ptr->num_reqd_args) && ! ef_ptr->internals_ptr->has_vari_args) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid argument index");
        return NULL;
    }
    if ( (axis < 0) || (axis > 3) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid axis index");
        return NULL;
    }

    /* Catch seg faults from indiscriminately calling this function */
    if ( setjmp(jumpbuffer) == 1 ) {
        signal(SIGSEGV, segv_handler);
        PyErr_SetString(PyExc_ValueError, "Invalid function call - probably not from a ferret external function call");
        return NULL;
    }
    segv_handler = signal(SIGSEGV, pyefcn_signal_handler);
    if ( segv_handler == SIG_ERR ) {
        PyErr_SetString(PyExc_ValueError, "Unable to catch SIGSEGV");
        return NULL;
    }

    /* Get the subscripts for all of the arguments */
    ef_get_arg_subscripts_(&id, steplo, stephi, incr);

    /* Restore the original segv handler */
    signal(SIGSEGV, segv_handler);

    /* Check the indices for the coordinates of the desired axis of the argument */
    if ( (steplo[arg][axis] == UNSPECIFIED_INT4) || (stephi[arg][axis] == UNSPECIFIED_INT4) ||
         ((steplo[arg][axis] == 1) && (stephi[arg][axis] == ABSTRACT_AXIS_LEN)) ) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    /* Create a NumPy float64 ndarray to get the memory for the coordinates */
    if ( incr[arg][axis] == 0 ) {
        if ( steplo[arg][axis] <= stephi[arg][axis] )
            incr[arg][axis] = 1;
        else
            incr[arg][axis] = -1;
    }
    shape[0] = (Py_ssize_t) ((stephi[arg][axis] - steplo[arg][axis] + incr[arg][axis]) / incr[arg][axis]);
    coords_ndarray = PyArray_SimpleNew(1, shape, NPY_DOUBLE);
    if ( coords_ndarray == NULL ) {
        return NULL;
    }

    /* Get the full range of world coordinates for the requested axis */
    lo = steplo[arg][axis];
    hi = stephi[arg][axis];
    arg++;
    axis++;
    ef_get_coordinates_(&id, &arg, &axis, &lo, &hi, (double *)PyArray_DATA(coords_ndarray));

    return coords_ndarray;
}


static char pyefcnGetAxisBoxSizesDocstring[] =
    "Returns the \"box sizes\", in \"world\" coordinate units, \n"
    "for an axis of an argument to an external function \n"
    "\n"
    "Required arguments: \n"
    "    id = <int>: the ferret id of the external function \n"
    "    arg = <int>: the index (zero based) of the argument (can use ARG1, ARG2, ..., ARG9) \n"
    "    axis = <int>: the index (zero based) of the axis (can use X_AXIS, Y_AXIS, Z_AXIS, T_AXIS) \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    a NumPy float32 ndarray containing the \"box sizes\", \n"
    "    or None if the values cannot be determined at the time this was called \n"
    "\n"
    "Raises: \n"
    "    ValueError if id, arg, or axis is invalid \n";

static PyObject *pyefcnGetAxisBoxSizes(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char      *argNames[] = {"id", "arg", "axis", NULL};
    int               id, arg, axis;
    ExternalFunction *ef_ptr;
    int               steplo[EF_MAX_COMPUTE_ARGS][4], stephi[EF_MAX_COMPUTE_ARGS][4], incr[EF_MAX_COMPUTE_ARGS][4];
    int               lo, hi;
    npy_intp          shape[1];
    PyObject         *sizes_ndarray;

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "iii", argNames, &id, &arg, &axis) )
        return NULL;

    /* Check for obvious errors in the arguments passed */
    ef_ptr = ef_ptr_from_id_ptr(&id);
    if ( (ef_ptr == NULL) || ! ef_ptr->already_have_internals ) {
        PyErr_SetString(PyExc_ValueError, "Invalid ferret external function id");
        return NULL;
    }
    if ( (arg < 0) || (arg >= EF_MAX_ARGS) ||
         ((arg >= ef_ptr->internals_ptr->num_reqd_args) && ! ef_ptr->internals_ptr->has_vari_args) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid argument index");
        return NULL;
    }
    if ( (axis < 0) || (axis > 3) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid axis index");
        return NULL;
    }

    /* Catch seg faults from indiscriminately calling this function */
    if ( setjmp(jumpbuffer) == 1 ) {
        signal(SIGSEGV, segv_handler);
        PyErr_SetString(PyExc_ValueError, "Invalid function call - probably not from a ferret external function call");
        return NULL;
    }
    segv_handler = signal(SIGSEGV, pyefcn_signal_handler);
    if ( segv_handler == SIG_ERR ) {
        PyErr_SetString(PyExc_ValueError, "Unable to catch SIGSEGV");
        return NULL;
    }

    /* Get the subscripts for all of the arguments */
    ef_get_arg_subscripts_(&id, steplo, stephi, incr);

    /* Restore the original segv handler */
    signal(SIGSEGV, segv_handler);

    /* Check the indices for the coordinates of the desired axis of the argument */
    if ( (steplo[arg][axis] == UNSPECIFIED_INT4) || (stephi[arg][axis] == UNSPECIFIED_INT4) ||
         ((steplo[arg][axis] == 1) && (stephi[arg][axis] == ABSTRACT_AXIS_LEN)) ) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    /* Create a NumPy float32 ndarray to get the memory for the box sizes */
    if ( incr[arg][axis] == 0 ) {
        if ( steplo[arg][axis] <= stephi[arg][axis] )
            incr[arg][axis] = 1;
        else
            incr[arg][axis] = -1;
    }
    shape[0] = (Py_ssize_t) ((stephi[arg][axis] - steplo[arg][axis] + incr[arg][axis]) / incr[arg][axis]);
    sizes_ndarray = PyArray_SimpleNew(1, shape, NPY_FLOAT);
    if ( sizes_ndarray == NULL ) {
        return NULL;
    }

    /* Get the full range of box sizes for the requested axis */
    lo = steplo[arg][axis];
    hi = stephi[arg][axis];
    arg++;
    axis++;
    ef_get_box_size_(&id, &arg, &axis, &lo, &hi, (float *)PyArray_DATA(sizes_ndarray));

    return sizes_ndarray;
}


static char pyefcnGetAxisBoxLimitsDocstring[] =
    "Returns the \"box limits\", in \"world\" coordinate units, \n"
    "for an axis of an argument to an external function \n"
    "\n"
    "Required arguments: \n"
    "    id = <int>: the ferret id of the external function \n"
    "    arg = <int>: the index (zero based) of the argument (can use ARG1, ARG2, ..., ARG9) \n"
    "    axis = <int>: the index (zero based) of the axis (can use X_AXIS, Y_AXIS, Z_AXIS, T_AXIS) \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    a tuple of two NumPy float64 ndarrays containing the low and high \"box limits\", \n"
    "    or None if the values cannot be determined at the time this was called \n"
    "\n"
    "Raises: \n"
    "    ValueError if id, arg, or axis is invalid \n";

static PyObject *pyefcnGetAxisBoxLimits(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char      *argNames[] = {"id", "arg", "axis", NULL};
    int               id, arg, axis;
    ExternalFunction *ef_ptr;
    int               steplo[EF_MAX_COMPUTE_ARGS][4], stephi[EF_MAX_COMPUTE_ARGS][4], incr[EF_MAX_COMPUTE_ARGS][4];
    int               lo, hi;
    npy_intp          shape[1];
    PyObject         *low_limits_ndarray, *high_limits_ndarray;

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "iii", argNames, &id, &arg, &axis) )
        return NULL;

    /* Check for obvious errors in the arguments passed */
    ef_ptr = ef_ptr_from_id_ptr(&id);
    if ( (ef_ptr == NULL) || ! ef_ptr->already_have_internals ) {
        PyErr_SetString(PyExc_ValueError, "Invalid ferret external function id");
        return NULL;
    }
    if ( (arg < 0) || (arg >= EF_MAX_ARGS) ||
         ((arg >= ef_ptr->internals_ptr->num_reqd_args) && ! ef_ptr->internals_ptr->has_vari_args) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid argument index");
        return NULL;
    }
    if ( (axis < 0) || (axis > 3) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid axis index");
        return NULL;
    }

    /* Catch seg faults from indiscriminately calling this function */
    if ( setjmp(jumpbuffer) == 1 ) {
        signal(SIGSEGV, segv_handler);
        PyErr_SetString(PyExc_ValueError, "Invalid function call - probably not from a ferret external function call");
        return NULL;
    }
    segv_handler = signal(SIGSEGV, pyefcn_signal_handler);
    if ( segv_handler == SIG_ERR ) {
        PyErr_SetString(PyExc_ValueError, "Unable to catch SIGSEGV");
        return NULL;
    }

    /* Get the subscripts for all of the arguments */
    ef_get_arg_subscripts_(&id, steplo, stephi, incr);

    /* Restore the original segv handler */
    signal(SIGSEGV, segv_handler);

    /* Check the indices for the coordinates of the desired axis of the argument */
    if ( (steplo[arg][axis] == UNSPECIFIED_INT4) || (stephi[arg][axis] == UNSPECIFIED_INT4) ||
         ((steplo[arg][axis] == 1) && (stephi[arg][axis] == ABSTRACT_AXIS_LEN)) ) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    /* Create two NumPy float32 ndarrays to get the memory for the box limits */
    if ( incr[arg][axis] == 0 ) {
        if ( steplo[arg][axis] <= stephi[arg][axis] )
            incr[arg][axis] = 1;
        else
            incr[arg][axis] = -1;
    }
    shape[0] = (Py_ssize_t) ((stephi[arg][axis] - steplo[arg][axis] + incr[arg][axis]) / incr[arg][axis]);
    low_limits_ndarray = PyArray_SimpleNew(1, shape, NPY_DOUBLE);
    if ( low_limits_ndarray == NULL ) {
        return NULL;
    }
    high_limits_ndarray = PyArray_SimpleNew(1, shape, NPY_DOUBLE);
    if ( high_limits_ndarray == NULL ) {
        Py_DECREF(low_limits_ndarray);
        return NULL;
    }

    /* Get the full range of box limits for the requested axis */
    lo = steplo[arg][axis];
    hi = stephi[arg][axis];
    arg++;
    axis++;
    ef_get_box_limits_(&id, &arg, &axis, &lo, &hi, (float *)PyArray_DATA(low_limits_ndarray), (float *)PyArray_DATA(high_limits_ndarray));

    return Py_BuildValue("NN", low_limits_ndarray, high_limits_ndarray); /* Steals the references to the two ndarrays */
}


static char pyefcnGetAxisInfoDocstring[] =
    "Returns information about the axis of an argument to an external function \n"
    "\n"
    "Required arguments: \n"
    "    id = <int>: the ferret id of the external function \n"
    "    arg = <int>: the index (zero based) of the argument (can use ARG1, ARG2, ..., ARG9) \n"
    "    axis = <int>: the index (zero based) of the axis (can use X_AXIS, Y_AXIS, Z_AXIS, T_AXIS) \n"
    "\n"
    "Optional arguments: \n"
    "    (none) \n"
    "\n"
    "Returns: \n"
    "    a dictionary defining the following keys: \n"
    "        \"name\": name string for the axis coordinate \n"
    "        \"unit\": name string for the axis unit \n"
    "        \"backwards\": boolean - reversed axis? \n"
    "        \"modulo\": boolean - periodic/wrapping axis? \n"
    "        \"regular\": boolean - evenly spaced axis? \n"
    "        \"size\": number of coordinates on this axis, or -1 if the value \n"
    "                  cannot be determined at the time this was called \n"
    "\n"
    "Raises: \n"
    "    ValueError if id, arg, or axis is invalid \n";

static PyObject *pyefcnGetAxisInfo(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char      *argNames[] = {"id", "arg", "axis", NULL};
    int               id, arg, axis;
    ExternalFunction *ef_ptr;
    int               steplo[EF_MAX_COMPUTE_ARGS][4], stephi[EF_MAX_COMPUTE_ARGS][4], incr[EF_MAX_COMPUTE_ARGS][4];
    int               num_coords;
    char              name[80];
    char              unit[80];
    int               backwards;
    int               modulo;
    int               regular;
    PyObject         *backwards_bool;
    PyObject         *modulo_bool;
    PyObject         *regular_bool;

    /* Parse the arguments, checking if an Exception was raised */
    if ( ! PyArg_ParseTupleAndKeywords(args, kwds, "iii", argNames, &id, &arg, &axis) )
        return NULL;

    /* Check for obvious errors in the arguments passed */
    ef_ptr = ef_ptr_from_id_ptr(&id);
    if ( (ef_ptr == NULL) || ! ef_ptr->already_have_internals ) {
        PyErr_SetString(PyExc_ValueError, "Invalid ferret external function id");
        return NULL;
    }
    if ( (arg < 0) || (arg >= EF_MAX_ARGS) ||
         ((arg >= ef_ptr->internals_ptr->num_reqd_args) && ! ef_ptr->internals_ptr->has_vari_args) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid argument index");
        return NULL;
    }
    if ( (axis < 0) || (axis > 3) ) {
        PyErr_SetString(PyExc_ValueError, "Invalid axis index");
        return NULL;
    }

    /* Catch seg faults from indiscriminately calling this function */
    if ( setjmp(jumpbuffer) == 1 ) {
        signal(SIGSEGV, segv_handler);
        PyErr_SetString(PyExc_ValueError, "Invalid function call - probably not from a ferret external function call");
        return NULL;
    }
    segv_handler = signal(SIGSEGV, pyefcn_signal_handler);
    if ( segv_handler == SIG_ERR ) {
        PyErr_SetString(PyExc_ValueError, "Unable to catch SIGSEGV");
        return NULL;
    }

    /* Get the subscripts for all of the arguments */
    ef_get_arg_subscripts_(&id, steplo, stephi, incr);

    /* Restore the original segv handler */
    signal(SIGSEGV, segv_handler);

    /* Check the indices for the coordinates of the desired axis of the argument */
    if ( (steplo[arg][axis] == UNSPECIFIED_INT4) || (stephi[arg][axis] == UNSPECIFIED_INT4) ||
         ((steplo[arg][axis] == 1) && (stephi[arg][axis] == ABSTRACT_AXIS_LEN)) ) {
        num_coords = -1;
    }
    else {
        if ( incr[arg][axis] == 0 ) {
            if ( steplo[arg][axis] <= stephi[arg][axis] )
                incr[arg][axis] = 1;
            else
                incr[arg][axis] = -1;
        }
        num_coords = (stephi[arg][axis] - steplo[arg][axis] + incr[arg][axis]) / incr[arg][axis];
    }

    /* Get the rest of the info */
    arg++;
    axis++;
    ef_get_single_axis_info_(&id, &arg, &axis, name, unit, &backwards, &modulo, &regular, 80, 80);

    /* Assign the Python bool objects */
    if ( backwards != 0 )
        backwards_bool = Py_True;
    else
        backwards_bool = Py_False;
    if ( modulo != 0 )
        modulo_bool = Py_True;
    else
        modulo_bool = Py_False;
    if ( regular != 0 )
        regular_bool = Py_True;
    else
        regular_bool = Py_False;

    /* Using O for the booleans to increment the references to these objects */
    return Py_BuildValue("{sssssOsOsOsi}", "name", name, "unit", unit,
                                           "backwards", backwards_bool, "modulo", modulo_bool,
                                           "regular", regular_bool, "size", num_coords);
}


/* List of Python functions and their docstrings available in this module */
static struct PyMethodDef pyferretMethods[] = {
    {"_start", (PyCFunction) pyferretStart, METH_VARARGS | METH_KEYWORDS, pyferretStartDocstring},
    {"_run", (PyCFunction) pyferretRunCommand, METH_VARARGS | METH_KEYWORDS, pyferretRunCommandDocstring},
    {"_get", (PyCFunction) pyferretGetData, METH_VARARGS | METH_KEYWORDS, pyferretGetDataDocstring},
    {"_put", (PyCFunction) pyferretPutData, METH_VARARGS | METH_KEYWORDS, pyferretPutDataDocstring},
    {"_resize", (PyCFunction) pyferretResizeMemory, METH_VARARGS | METH_KEYWORDS, pyferretResizeMemoryDocstring},
    {"_stop", (PyCFunction) pyferretStop, METH_NOARGS, pyferretStopDocstring},
    {"_get_axis_coordinates", (PyCFunction) pyefcnGetAxisCoordinates, METH_VARARGS | METH_KEYWORDS, pyefcnGetAxisCoordinatesDocstring},
    {"_get_axis_box_sizes", (PyCFunction) pyefcnGetAxisBoxSizes, METH_VARARGS | METH_KEYWORDS, pyefcnGetAxisBoxSizesDocstring},
    {"_get_axis_box_limits", (PyCFunction) pyefcnGetAxisBoxLimits, METH_VARARGS | METH_KEYWORDS, pyefcnGetAxisBoxLimitsDocstring},
    {"_get_axis_info", (PyCFunction) pyefcnGetAxisInfo, METH_VARARGS | METH_KEYWORDS, pyefcnGetAxisInfoDocstring},
    {NULL, (PyCFunction) NULL, 0, NULL}
};

static char pyferretModuleDocstring[] =
"An extension module enabling the use of Ferret from Python \n";

/* For the _pyferret module, this function must be named init_pyferret */
PyMODINIT_FUNC init_pyferret(void)
{
    char names[64][32];
    int  values[64];
    int  numvals;
    int  k;

    /* Create the module with the indicated methods */
    PyObject *mod = Py_InitModule3("_pyferret", pyferretMethods, pyferretModuleDocstring);

    /* Add ferret parameter values */
    get_ferret_params_(names, values, &numvals);
    for (k = 0; k < numvals; k++) {
        PyModule_AddIntConstant(mod, names[k], values[k]);
    }

    /* Add parameters for the python axis functions */
    PyModule_AddIntConstant(mod, "X_AXIS", 0);
    PyModule_AddIntConstant(mod, "Y_AXIS", 1);
    PyModule_AddIntConstant(mod, "Z_AXIS", 2);
    PyModule_AddIntConstant(mod, "T_AXIS", 3);
    PyModule_AddIntConstant(mod, "ARG1", 0);
    PyModule_AddIntConstant(mod, "ARG2", 1);
    PyModule_AddIntConstant(mod, "ARG3", 2);
    PyModule_AddIntConstant(mod, "ARG4", 3);
    PyModule_AddIntConstant(mod, "ARG5", 4);
    PyModule_AddIntConstant(mod, "ARG6", 5);
    PyModule_AddIntConstant(mod, "ARG7", 6);
    PyModule_AddIntConstant(mod, "ARG8", 7);
    PyModule_AddIntConstant(mod, "ARG9", 8);

    /* Parameters for interpreting axis data */
    PyModule_AddIntConstant(mod, "AXISTYPE_LONGITUDE",    AXISTYPE_LONGITUDE);
    PyModule_AddIntConstant(mod, "AXISTYPE_LATITUDE",     AXISTYPE_LATITUDE);
    PyModule_AddIntConstant(mod, "AXISTYPE_LEVEL",        AXISTYPE_LEVEL);
    PyModule_AddIntConstant(mod, "AXISTYPE_TIME",         AXISTYPE_TIME);
    PyModule_AddIntConstant(mod, "AXISTYPE_CUSTOM",       AXISTYPE_CUSTOM);
    PyModule_AddIntConstant(mod, "AXISTYPE_ABSTRACT",     AXISTYPE_ABSTRACT);
    PyModule_AddIntConstant(mod, "AXISTYPE_NORMAL",       AXISTYPE_NORMAL);
    PyModule_AddIntConstant(mod, "TIMEARRAY_DAYINDEX",    TIMEARRAY_DAYINDEX);
    PyModule_AddIntConstant(mod, "TIMEARRAY_MONTHINDEX",  TIMEARRAY_MONTHINDEX);
    PyModule_AddIntConstant(mod, "TIMEARRAY_YEARINDEX",   TIMEARRAY_YEARINDEX);
    PyModule_AddIntConstant(mod, "TIMEARRAY_HOURINDEX",   TIMEARRAY_HOURINDEX);
    PyModule_AddIntConstant(mod, "TIMEARRAY_MINUTEINDEX", TIMEARRAY_MINUTEINDEX);
    PyModule_AddIntConstant(mod, "TIMEARRAY_SECONDINDEX", TIMEARRAY_SECONDINDEX);

    /* Private parameter return value from _pyferret._run indicating the program should shut down */
    PyModule_AddIntConstant(mod, "_FERR_EXIT_PROGRAM", FERR_EXIT_PROGRAM);
}
