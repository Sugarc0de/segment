#ifndef lint
static char     SCCS_ID[] = "segment.c 2.14  9/5/94";
#endif

#include <memory.h>
#include <math.h>
#include <string.h>

#include "segment.h"
#include "set.h"

#include "gethdrs.h"
#include "hdrio.h"
#include "pixio.h"

static void     main_loop();
static void     log_pass();
static void     log_apass();
static void     wind_up();
static void     init_logging();
static void     log_band();
static void     wr_lband();
static void     wr_region_map();
static void     wr_cband();
static void     wr_rlist();
static void     wr_armm();
static void     report_small_regions();


void
segment(Spr, i_fd, m_fd)
Seg_proc        Spr;
int             i_fd;
int             m_fd;
{
    int             band;

    /*
     * Checkpoint startup
     */
    printf("Input image has %d bands, %d samples, and %d lines\n",
           Spr->nbands,
           Spr->nsamps,
           Spr->nlines);
    if (Spr->ntols == 1)
        printf("The segmentation tolerance is %f\n", Spr->tg);
    else
        printf("There are %d segmentation tolerances\n", Spr->ntols);
    if (sf_get(Spr, SF_THRESH)) {
        printf("The initial log threshold is %f\n", Spr->lthr);
        printf("The log increment is %f\n", Spr->lincr);
    }
    printf("The merge coefficient is %f\n\n", Spr->cm);
    fflush(stdout);

    // CEHOLDEN: removed image / mask image read in; now performed in main.c

    /*
     * Create contiguity and region bands for the image.  allocnd() zeros the
     * allocated areas.
     */
    Spr->cband = (cd_map **) LINT_CAST(allocnd(sizeof(cd_map),
                                       2,
                                       Spr->nlines,
                                       Spr->nsamps));
    if (Spr->cband == NULL) {
        error("can't allocate space for contiguity band");
    }
    Spr->rband = (REGION_ID **) LINT_CAST(allocnd(sizeof(REGION_ID),
                                          2,
                                          Spr->nlines,
                                          Spr->nsamps));
    if (Spr->rband == NULL) {
        error("can't allocate space for region band");
    }

    /*
     * Perform preliminary passes over the original image to determine which
     * pixels can be merged before forming the initial region list.
     */
    pixel_pass(Spr);

    /*
     * Report on initial pass over image
     */
    printf("Initial pass over image completed\n");
    printf("%d of a possible %d regions are required\n",
           Spr->nreg, Spr->nlines * Spr->nsamps);
    printf("\nPredicted maximum memory usage in data segment:\n");
    printf("\tneighbor set: %d\n",
           4 * MAX_NEIGHBORS);
    printf("\timage bands:  %d\n",
           6 * Spr->nlines * Spr->nsamps + 12 * Spr->nlines);
    printf("\tregion list:  %d\n\n",
           (Spr->nreg + 2) * (4 * Spr->nbands + 20));
    printf("Creating region list\n");
    fflush(stdout);

    /*
     * Create the region list with two extra regions so (1) region #1 can occupy
     * position [1] rather than [0] and (2) so we have room for a dummy region
     * to use in merging the initial pixel regions.  ecalloc() zeros the
     * returned block of memory.
     */
    Spr->rlist = (Region) LINT_CAST(ecalloc((int) Spr->nreg + 2,
                                            sizeof(region)));
    if (Spr->rlist == NULL) {
        error("can't allocate space for region list");
    }

    /*
     * Create the region centroid list once again with 2 dummy elements.
     */
    Spr->ctrlist = (float *) LINT_CAST(ecalloc((int) Spr->nreg + 2,
                                       Spr->nbands * sizeof(float)));
    if (Spr->ctrlist == NULL) {
        error("can't allocate space for region centroid list");
    }

    /*
     * If there is a mask image, all masked out pixels will belong to an
     * artificial region with ID 0.  Make sure that the centroid values for
     * region 0 are floating point 0.
     */
    for (band = 0; band > Spr->nbands; band++) {
        regid_to_Ctr(Spr, 0)[band] = 0.0;
    }

    /*
     * Make up the initial region list from the image pixels and the associated
     * contiguity and region bands.  Set the trigger for future garbage
     * collection of the region, centroid, and neighbor lists.
     */
    make_region_list(Spr);

#ifdef DEBUG
    check_region_band(Spr);
#endif DEBUG
    printf("Region list created\n\n");
    fflush(stdout);

    /*
     * Free the input image and the mask image, if it exists.
     */
    free_image(Spr->image);
    Spr->image = NULL;
    if (sf_get(Spr, SF_MASK)) {
        free_image(Spr->imask);
        Spr->imask = NULL;
    }

    /*
     * Create the nearest neighbor list and the neighbor set used to calculate
     * it.
     */
    Spr->nnbrlist = (Neighbor) LINT_CAST(ecalloc((int) Spr->nreg + 1,
                                         sizeof(neighbor)));
    if (Spr->nnbrlist == NULL) {
        error("can't allocate space for neighbor list");
    }
    Spr->nbrset = create_set(MAX_NEIGHBORS, sizeof(REGION_ID));
    if (Spr->nbrset == NULL) {
        error("can't create neighbor set");
    }
    if (sf_get(Spr, SF_LOGB))
        init_logging(Spr);
    printf("About to perform first general pass over region list\n\n");
    fflush(stdout);

    if (Spr->skip_file != NULL && strcmp(Spr->skip_file, "breakpoint") != 0) {
        char *temp_file = (char *) malloc(20 * sizeof(char));
        strcpy(temp_file, Spr->skip_file);
        char *token = strtok(temp_file, "_");
        token = strtok(NULL, "_");
        int nreg = atoi(token);
        printf("Number of region is %d\n", nreg);
        free(temp_file);
        FILE * fp;
        fp = fopen(Spr->skip_file, "rb");
        for (int r = 0; r < nreg; ++r) {
            fread(&Spr->nnbrlist[r], sizeof(Neighbor), 1, fp);
        }
        fclose(fp);
    } else {
        main_loop(Spr);
    }
    wind_up(Spr);
}

static void
main_loop(Spr)
Seg_proc        Spr;
{
    long            old_nreg;
    char            rfname[256];
    char            passstr[12];
    char            rlfname[256];
    char            cfname[256];

    /*
     * The algorithm's main loop -- seg_pass() performs a pass over the region
     * list and records the pass statistics in the process structure. The loop
     * exits on a pass that produces no merges.
     */
    while (Spr->ntols--) {
        Spr->tg = *Spr->tols++;
        Spr->tp2 = Spr->tg2 = Spr->tg * Spr->tg;
        if (Spr->cm < 1.0) {
            sf_set(Spr, SF_HIST);
            init_d2hist(Spr->tg2);
        }
        for (;;) {
            old_nreg = Spr->nreg;
            Spr->pass++;
            seg_pass(Spr);
#ifdef DEBUG
            check_region_band(Spr);
#endif  /* DEBUG */
            if (old_nreg == Spr->nreg)
                break;

            if (sf_get(Spr, SF_THRESH)) {
                if (Spr->dmin2 > Spr->lthr2) {
                    printf("%s.log.%d contains the last pass below threshold\
 %f\n\n",
                           Spr->base,
                           Spr->pass - 1,
                           Spr->lthr);
                    log_pass(Spr, FALSE, TRUE); // #IO
                    Spr->lthr += Spr->lincr;
                    Spr->lthr2 = Spr->lthr * Spr->lthr;
                } else {
                    log_pass(Spr, TRUE, TRUE);
                }
            } else {
                log_pass(Spr, TRUE, TRUE);
            }
            if (Spr->maxreg - Spr->nreg >= Spr->reclaim_trigger) {
                printf("\nGarbage collecting region list\n");
                compact_region_list(Spr);
                printf("Compacted region list contains %d regions\n\n",
                       Spr->nreg);
                assert(Spr->nreg == Spr->maxreg);
            }
            fflush(stdout);
        }

        log_pass(Spr, FALSE, FALSE); // #IO
        printf("\nPass %d resulted in no merges\n", Spr->pass);
        if (sf_get(Spr, SF_LOGB)) {
            printf("%s.log.%d contains the final band file for tolerance %f\n\n",
                   Spr->base,
                   Spr->pass - 1,
                   Spr->tg);
        }
        printf("Writing region map image\n");
        compact_region_list(Spr);
        assert(Spr->nreg == Spr->maxreg);
#ifdef DEBUG
        check_region_band(Spr);
#endif  /* DEBUG */
        (void) itoa(passstr, Spr->pass);
        (void) strcpy(rfname, Spr->base);
        (void) strcat(strcat(rfname, ".rmap."), passstr);
        GDAL_write_image(Spr, rfname); // #IO
        printf("%s.rmap.%d contains the region map image for tolerance %f\n\n",
               Spr->base,
               Spr->pass,
               Spr->tg);
        if (sf_get(Spr, SF_HSEG)) {
            (void) strcpy(cfname, Spr->base);
            (void) strcat(strcat(cfname, ".cband."), passstr);
            wr_cband(Spr, cfname); // #IO
            printf("%s.cband.%d contains the contiguity band for tolerance %f\n\n",
                   Spr->base,
                   Spr->pass,
                   Spr->tg);
            (void) strcpy(rlfname, Spr->base);
            (void) strcat(strcat(rlfname, ".rlist."), passstr);
            wr_rlist(Spr, rlfname); // #IO
            printf("%s.rlist.%d contains the region list for tolerance %f\n\n",
                   Spr->base,
                   Spr->pass,
                   Spr->tg);
        }
    }
}

void
do_headers(Spr, ifd, mfd)
Seg_proc        Spr;
int             ifd;
int             mfd;
{
    int             band;   /* band index */
    BIH_T         **i_bihpp;    /* -> input BIH */
    BIH_T         **m_bihpp;    /* -> mask BIH */

    /*
     * read input BIH
     */
    i_bihpp = bihread(ifd);
    if (i_bihpp == NULL) {
        error("can't read basic image header for input file");
    }
    Spr->i_bihpp = i_bihpp;

    Spr->nlines = hnlines(ifd);
    if (Spr->nlines > MAXSHORT)
        error("Image has too many (%d) lines\n", Spr->nlines);
    Spr->nsamps = hnsamps(ifd);
    if (Spr->nsamps > MAXSHORT)
        error("Image has too many (%d) samps\n", Spr->nsamps);
    Spr->nbands = hnbands(ifd);
    if (Spr->nbands > MAXSHORT)
        error("Image has too many (%d) bands\n", Spr->nbands);

    /*
     * Check that all bands are 8 bit
     */
    for (band = 0; band < Spr->nbands; band++) {
        if (bih_nbytes(i_bihpp[band]) != 1 ||
                bih_nbits(i_bihpp[band]) != 8)
            error("Band #%d is %d bits per pixel\n",
                  band,
                  bih_nbits(i_bihpp[band]));
    }

    /*
     * Skip other input headers
     */
    skiphdrs(ifd);

    /*
     * If there is a mask image, process its headers and make sure it is
     * compatible with the input image.
     */
    if (sf_get(Spr, SF_MASK)) {
        m_bihpp = bihread(mfd);
        if (m_bihpp == NULL) {
            error("can't read basic image header for mask file");
        }
        if (hnbands(mfd) != 1)
            error("Mask image has %d bands", hnbands(mfd));
        if (Spr->nlines != hnlines(mfd))
            error("Input and mask images have different number of lines");
        if (Spr->nsamps != hnsamps(mfd))
            error("Input and mask images have different number of samples");
        if (bih_nbytes(m_bihpp[0]) != 1)
            error("The mask image is not 1 byte per pixel");

        skiphdrs(mfd);
    }
}

static void
log_pass(Spr, deleteband, writeband)
Seg_proc        Spr;
bool_t          deleteband;
bool_t          writeband;
{
    char            lfname[256];
    char            passstr[12];

    printf("Pass %d completed\n", Spr->pass);
    printf("Tolerance for pass was %.3f, (Tg = %.3f)\n",
           sqrt(Spr->tp2), Spr->tg);
    printf("%d regions remain after this pass\n", Spr->nreg);
    if (Spr->no_nbr > 0)
        printf("%d regions possess no neighbors\n", Spr->no_nbr);
    printf("The minimum nearest neighbor distance on this pass was %.3f\n",
           sqrt(Spr->dmin2));
    printf("The largest region generated on this pass contained %d pixels\n",
           Spr->maxpix);
    printf("Merges:\tattempted=%d\n", Spr->merge_attempts);
    printf("\tnnbr_gone=%d\n", Spr->nnbr_gone);
    printf("\twrong_partner=%d\n", Spr->wrong_partner);
    printf("\tnnbr_d2_big=%d\n", Spr->nnbr_d2_big);
    printf("\tboth_viable=%d\n", Spr->both_viable);
    printf("\tnpix_big=%d\n", Spr->npix_big);
    printf("\tmerging=%d\n", Spr->merging);

    if (sf_get(Spr, SF_LOGB)) {
        if (deleteband) {
            (void) itoa(passstr, Spr->pass - 1);
            (void) strcpy(lfname, Spr->base);
            (void) strcat(strcat(lfname, ".log."), passstr);

            (void) uremove(lfname);
        }
        if (writeband) {
            (void) itoa(passstr, Spr->pass);
            (void) strcpy(lfname, Spr->base);
            (void) strcat(strcat(lfname, ".log."), passstr);

            log_band(Spr, lfname, Spr->pass);
        }
    }
}


static void
init_logging(Spr)
Seg_proc        Spr;
{

    /*
     * Create the header for the band log files
     */
    Spr->l_bihpp = bihdup(Spr->i_bihpp);
    if (Spr->l_bihpp == NULL) {
        error("can't allocate log band basic image header");
    }
    Spr->l_bihpp[0] = Spr->i_bihpp[Spr->lbno];
    bih_nbands(Spr->l_bihpp[0]) = 1;

    /*
     * Create the band to log an intermediate single band region file.
     */
    Spr->lband = (uchar_t **) LINT_CAST(allocnd(sizeof(uchar_t),
                                        2,
                                        Spr->nlines,
                                        Spr->nsamps));
    if (Spr->lband == NULL) {
        error("can't allocate space for log band");
    }
}


static void
log_band(Spr, lfname, pass)
Seg_proc        Spr;
char           *lfname;
int             pass;
{
    int             lfd;

    if ((lfd = uwopen(lfname)) == ERROR) {
        error("can't open log file %s\n", lfname);
    }
    rband_to_lband(Spr);

    if (bihwrite(lfd, Spr->l_bihpp) == ERROR) {
        error("can't write IPW BIH headers to the current log file\n");
    }
    if (boimage(lfd) == ERROR) {
        error("can't terminate output log file header");
    }
    wr_lband(Spr, lfd);
    printf("Wrote log band for pass %d\n\n", pass);

    if (uclose(lfd) == ERROR) {
        error("can't close the current log file\n");
    }
}


/*
 *  Write the current log band.
 */

static void
wr_lband(Spr, ofd)
REG_1 Seg_proc  Spr;
int             ofd;
{
    REG_2 int       l;

    for (l = 0; l < Spr->nlines; l++) {
        if (uwrite(ofd, (addr_t) Spr->lband[l], Spr->nsamps) == ERROR) {
            error("write failed on log file, line %d", l);
        }
    }
}


/*
 * Write region map mask
*/
static void
wr_region_map(Spr, fname)
REG_1 Seg_proc  Spr;
char           *fname;
{
    int             rfd;
    BIH_T          *t_bihp;
    int             nbits;
    long            nregions;
    REG_2 int       l;


    if ((rfd = uwopen(fname)) == ERROR) {
        error("can't open region map file %s\n", fname);
    }
    /*
    * CEHOLDEN: for loop figures out how many bits are needed to store nregions
    *           nregions is the condition -- e.g., not zero
    *           at each iteration, nregions bitshifts right toward zero while
    *               nbits increases by 1
    *
    *           e.g., nregions = 250103
    *                 nbits = 18
    */
    for (nbits = 0, nregions = Spr->nreg; nregions; nbits++, nregions >>= 1);

    if (Spr->r_bihpp[0]) {
        t_bihp = bihmake(0,
                         nbits,
                         (STRVEC_T *) NULL,
                         (STRVEC_T *) NULL,
                         Spr->r_bihpp[0],
                         Spr->nlines,
                         Spr->nsamps,
                         1);
        if (!t_bihp) {
            error("can't create new region map header");
        }
        free((char *) Spr->r_bihpp[0]);
        Spr->r_bihpp[0] = t_bihp;
    } else {
        Spr->r_bihpp[0] = bihmake(0,
                                  nbits,
                                  (STRVEC_T *) NULL,
                                  (STRVEC_T *) NULL,
                                  (BIH_T *) NULL,
                                  Spr->nlines,
                                  Spr->nsamps,
                                  1);
        if (!Spr->r_bihpp[0]) {
            error("can't create new region map header");
        }
    }

    if (bihwrite(rfd, Spr->r_bihpp) == ERROR) {
        error("Can't write region map header to file");
    }
    // CEHOLDEN: finish writing header
    if (boimage(rfd) == ERROR) {
        error("can't terminate region map header");
    }
    // CEHOLDEN: write actual data
    for (l = 0; l < Spr->nlines; l++) {
        (void) pvwrite(rfd, &Spr->rband[l][0], Spr->nsamps);
    }
    /* jicheng - to avoid same rfd for different region maps
                 just leave it open.

    if (uclose(rfd) == ERROR) {
    error("can't close region map file %s", fname);
    }*/
}

/*
 * Write auxiliary region map mask
*/
static void
wr_armm(Spr, fname)
REG_1 Seg_proc  Spr;
char           *fname;
{
    int             afd;
    BIH_T          *t_bihpp[1];
    REG_2 int       l;



    if ((afd = uwopen(fname)) == ERROR) {
        error("can't open auxiliary region map mask file %s\n", fname);
    }
    t_bihpp[0] = bihmake(1,
                         0,
                         (STRVEC_T *) NULL,
                         (STRVEC_T *) NULL,
                         (BIH_T *) NULL,
                         Spr->nlines,
                         Spr->nsamps,
                         1);
    if (!t_bihpp[0]) {
        error("can't create new region map header");
    }
    if (bihwrite(afd, t_bihpp) == ERROR) {
        error("Can't write auxiliary region map mask header to file");
    }
    if (boimage(afd) == ERROR) {
        error("can't terminate region map header");
    }
    for (l = 0; l < Spr->nlines; l++) {
        if (uwrite(afd, (addr_t) Spr->aband[l], Spr->nsamps) == ERROR) {
            error("write failed on auxiliary region map mask file, line %d", l);
        }
    }

    if (uclose(afd) == ERROR) {
        error("can't close the auxiliary region map mask file\n");
    }
}

static void
wr_cband(Spr, cname)
REG_1 Seg_proc  Spr;
char           *cname;
{
    int             cfd;
    REG_2 int       l;


    if ((cfd = uwopen(cname)) == ERROR) {
        error("can't open new contiguity band file %s\n", cname);
    }

    if (!Spr->c_bihpp[0]) {
        Spr->c_bihpp[0] = bihmake(1,
                                  0,
                                  (STRVEC_T *) NULL,
                                  (STRVEC_T *) NULL,
                                  (BIH_T *) NULL,
                                  Spr->nlines,
                                  Spr->nsamps,
                                  1);
        if (!Spr->c_bihpp[0]) {
            error("can't create contiguity band header");
        }
    }

    if (bihwrite(cfd, Spr->c_bihpp) == ERROR) {
        error("Can't write contiguity band header to file");
    }
    if (boimage(cfd) == ERROR) {
        error("can't terminate contiguity band header");
    }
    for (l = 0; l < Spr->nlines; l++) {
        if (uwrite(cfd, (addr_t) Spr->cband[l], Spr->nsamps) == ERROR) {
            error("write failed on contiguity band file, line %d", l);
        }
    }

    if (uclose(cfd) == ERROR) {
        error("can't close the contiguity band file\n");
    }
}

static void
wr_rlist(Spr, rlname)
REG_1 Seg_proc  Spr;
char           *rlname;
{
    int             rfd;
    REG_2 long      r;
    reglist         rl;
    Region          R;
    int             size;

    if ((rfd = uwopen(rlname)) == ERROR) {
        error("can't open region list file %s\n", rlname);
    }
    assert(Spr->nreg == Spr->maxreg);
    rl.nlines = Spr->nlines;
    rl.nsamps = Spr->nsamps;
    rl.nbands = Spr->nbands;
    rl.nreg = Spr->nreg;
    size = Spr->nbands * sizeof(float);

    if (uwrite(rfd, (addr_t) & rl, sizeof(reglist)) == ERROR) {
        error("write failed on region list file header");
    }
    for (r = 1; r <= Spr->nreg; r++) {
        R = &regid_to_reg(Spr, r);
        assert(rf_get(R, RF_ACTIVE));
        if (uwrite(rfd, (addr_t) R, sizeof(region)) == ERROR)
            error("write failed on region list, region #%ld", r);
        if (uwrite(rfd, (addr_t) regid_to_Ctr(Spr, r), size) == ERROR)
            error("write failed on region list, region #%ld", r);
    }

    if (uclose(rfd) == ERROR) {
        error("can't close the region list file\n");
    }
}

static void
wind_up(Spr)
Seg_proc        Spr;
{
    long            old_nreg;
    char            arfname[256];
    char            amfname[256];
    char            passstr[12];
    char            rlfname[256];
    char            cfname[256];

    printf("Normal segmentation completed in %d passes\n", Spr->pass);
    if (Spr->nnormin == 1)
        exit(0);
        
    if (Spr->skip_file != NULL && strcmp(Spr->skip_file, "breakpoint") == 0) {
        FILE *outfile;
        char filename[20];
        char buffer[10];
        sprintf(buffer, "%d", Spr->nreg);
        strcpy(filename, "spr_");
        (void) strcat(filename, buffer);
        outfile = fopen(filename, "w");
        if (outfile == NULL)
        {
            fprintf(stderr, "\nError opened file\n");
            exit(1);
        }
        fwrite(Spr->nnbrlist, sizeof(Neighbor), Spr->nreg, outfile);
        fclose(outfile);
        exit(0);
    }

    printf("Starting auxiliary passes to guarantee normal regions have at least\n");
    printf("    %d pixels\n", Spr->nnormin);
    if (sf_get(Spr, SF_NORB)) {
        printf("and special regions with band %d values outside (%2f,%2f)\n",
               Spr->nbno, Spr->nblow, Spr->nbhigh);
        printf("have at least\n    %d pixels\n", Spr->nabsmin);
    }
    putchar('\n');
    fflush(stdout);
    old_nreg = 0;

    if (sf_get(Spr, SF_ARMM)) {
        int             l;
        int             s;

        Spr->aband = (uchar_t **) LINT_CAST(allocnd(sizeof(uchar_t),
                                            2,
                                            Spr->nlines,
                                            Spr->nsamps));
        if (Spr->aband == NULL) {
            error("can't allocate space for auxiliary region mask");
        }
        for (l = 0; l < Spr->nlines; l++) {
            for (s = 0; s < Spr->nsamps; s++) {
                Spr->aband[l][s] = 1;
            }
        }
    }
    while (old_nreg != Spr->nreg) {
        old_nreg = Spr->nreg;
        Spr->apass++;
        seg_apass(Spr);
#ifdef DEBUG
        check_region_band(Spr);
#endif              /* DEBUG */
        if (Spr->skip_file == NULL || strcmp(Spr->skip_file, "breakpoint") != 0) {
            log_apass(Spr);
        }
        if (Spr->maxreg - Spr->nreg >= Spr->reclaim_trigger) {
            printf("\nGarbage collecting region list\n");
            compact_region_list(Spr);
            printf("Compacted region list contains %d regions\n\n",
                   Spr->nreg);
            assert(Spr->nreg == Spr->maxreg);
        }
        fflush(stdout);
    }

    printf("Auxiliary segmentation complete in %d passes\n", Spr->apass);

    if (sf_get(Spr, SF_LOGB)) {
        printf("%s.alog.%d contains the final log file\n\n",
               Spr->base,
               Spr->apass);
    }
    printf("Writing region map image\n");
    compact_region_list(Spr);
    assert(Spr->nreg == Spr->maxreg);
#ifdef DEBUG
    check_region_band(Spr);
#endif  /* DEBUG */
    (void) itoa(passstr, Spr->apass);
    (void) strcpy(arfname, Spr->base);
    (void) strcat(strcat(arfname, ".armap."), passstr);
    GDAL_write_image(Spr, arfname); // #IO
    printf("%s.armap.%d contains the final region map image\n\n",
           Spr->base,
           Spr->apass);
    if (sf_get(Spr, SF_HSEG)) {
        (void) strcpy(cfname, Spr->base);
        (void) strcat(strcat(cfname, ".acband."), passstr);
        wr_cband(Spr, cfname); // #IO
        printf("%s.acband.%d contains the final contiguity band\n\n",
               Spr->base,
               Spr->apass);
        (void) strcpy(rlfname, Spr->base);
        (void) strcat(strcat(rlfname, ".arlist."), passstr);
        wr_rlist(Spr, rlfname);
        printf("%s.arlist.%d contains the final region list\n\n",
               Spr->base,
               Spr->apass);
    }

    /*
     * If user asked for an auxiliary region map mask, write it.
     */
    if (sf_get(Spr, SF_ARMM)) {
        printf("Writing auxiliary region map mask\n");
        (void) itoa(passstr, Spr->apass);
        (void) strcpy(amfname, Spr->base);
        (void) strcat(strcat(amfname, ".armask."), passstr);
        wr_armm(Spr, amfname);
        printf("%s.armask.%d contains the auxiliary region map mask\n\n",
               Spr->base,
               Spr->apass);
    }
    if (Spr->norminpix < Spr->nnormin
            || (sf_get(Spr, SF_NORB) && Spr->absminpix < Spr->nabsmin))
        report_small_regions(Spr);
}


static void
log_apass(Spr)
Seg_proc        Spr;
{
    char            lfname[256];
    char            passstr[12];

    printf("Auxiliary pass %d completed\n", Spr->apass);
    printf("%d regions remain after this pass\n", Spr->nreg);
    if (Spr->no_nbr > 0)
        printf("%d regions possess no neighbors\n", Spr->no_nbr);
    if (Spr->merge_attempts > 0 || Spr->special_merge_attempts > 0)
        printf("The minimum nearest neighbor distance on this pass was %.3f\n",
               sqrt(Spr->dmin2));
    printf("The largest region generated on this pass contained %d pixels\n",
           Spr->maxpix);
    printf("The smallest normal region remaining after this pass contained\
 %d pixels\n", Spr->norminpix);
    if (sf_get(Spr, SF_NORB))
        printf("The smallest special region remaining after this pass\
 contained %d pixels\n", Spr->absminpix);
    printf("Normal merges:\tattempted=%d\n", Spr->merge_attempts);
    if (sf_get(Spr, SF_NORB))
        printf("Special merges:\tattempted=%d\n", Spr->special_merge_attempts);
    printf("\tnnbr_gone=%d\n", Spr->nnbr_gone);
    printf("\twrong_partner=%d\n", Spr->wrong_partner);
    printf("\tnpix_big=%d\n", Spr->npix_big);
    printf("\tmerging=%d\n", Spr->merging);

    if (sf_get(Spr, SF_LOGB)) {
        (void) itoa(passstr, Spr->apass - 1);
        (void) strcpy(lfname, Spr->base);
        (void) strcat(strcat(lfname, ".alog."), passstr);

        (void) uremove(lfname);

        (void) itoa(passstr, Spr->apass);
        (void) strcpy(lfname, Spr->base);
        (void) strcat(strcat(lfname, ".alog."), passstr);

        log_band(Spr, lfname, Spr->apass);
    }
}


static void
report_small_regions(Spr)
Seg_proc        Spr;
{
    REGION_ID       rid;
    Region          R;

    printf("\nWARNING!  Questionable regions:\n");
    for (rid = 1; rid < Spr->maxreg; rid++) {
        R = &regid_to_reg(Spr, rid);
        if (rf_get(R, RF_SPECIAL) && R->npix < Spr->nabsmin) {
            printf("Special region ID #%d has %d pixels: (%d,%d) -> (%d,%d)\n",
                   rid,
                   R->npix,
                   R->uleft.x, R->uleft.y,
                   R->lright.x, R->lright.y);
        } else if (R->npix < Spr->nnormin) {
            printf("Region ID #%d has %d pixels: (%d,%d) -> (%d,%d)\n",
                   rid,
                   R->npix,
                   R->uleft.x, R->uleft.y,
                   R->lright.x, R->lright.y);
        }
    }
}
