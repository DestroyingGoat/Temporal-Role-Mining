#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- Configuration ---
#define MAX_LINE 8192
#define MAX_PATH 1024
#define EPSILON 1e-6 
#define BASE_DIR "./" 
#define INPUT_UPA "apj.txt"
#define INPUT_TIME "TIME_10int_c25_o75_apj.txt" 

#define OUTPUT_UA "UA_TIME.txt"
#define OUTPUT_PA "PA_TIME.txt"
#define OUTPUT_REB "REB_TIME.txt"

// --- Structures ---

typedef struct {
    double start;
    double end;
} Interval;

typedef struct {
    int uid;
    int pid;
    Interval t;
    int id;         
    int covered;
} TupaEntry;

typedef struct {
    TupaEntry *entries;
    int count;
    int capacity;
    int num_users;
    int num_perms;
    int **user_map; 
    int *user_map_counts;
} Dataset;

typedef struct {
    int *uids;
    int num_uids;
    int *pids;
    int num_pids;
    Interval t;
    int id;
    
    // Optimization: Keep sorted for fast set operations
    int uids_capacity;
    int pids_capacity;

    // For algo use
    int *temp_covered_indices;
    int temp_num_covered;
    int split_count;
    int overlap_count;
    int valid; 
} Role;

// --- Helper Functions ---

double max_d(double a, double b) { return (a > b) ? a : b; }
double min_d(double a, double b) { return (a < b) ? a : b; }

double duration(Interval iv) {
    double d = iv.end - iv.start;
    return (d < 0) ? 0.0 : d;
}

int is_subset(Interval container, Interval sub) {
    return (sub.start >= container.start - EPSILON) && 
           (sub.end <= container.end + EPSILON);
}

int compare_ints(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

void add_to_array(int **arr, int *count, int *capacity, int val) {
    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? 8 : (*capacity * 2);
        int *new_arr = realloc(*arr, *capacity * sizeof(int));
        if (!new_arr) { perror("realloc failed"); exit(1); }
        *arr = new_arr;
    }
    (*arr)[*count] = val;
    (*count)++;
}

void add_unique(int **arr, int *count, int *capacity, int val) {
    for(int i=0; i<*count; i++) {
        if ((*arr)[i] == val) return;
    }
    add_to_array(arr, count, capacity, val);
}

void union_sorted_sets(Role *r1, Role *r2, int merge_pids) {
    if (merge_pids) {
        for(int i=0; i<r2->num_pids; i++) {
            add_unique(&r1->pids, &r1->num_pids, &r1->pids_capacity, r2->pids[i]);
        }
        qsort(r1->pids, r1->num_pids, sizeof(int), compare_ints);
    } else {
        for(int i=0; i<r2->num_uids; i++) {
            add_unique(&r1->uids, &r1->num_uids, &r1->uids_capacity, r2->uids[i]);
        }
        qsort(r1->uids, r1->num_uids, sizeof(int), compare_ints);
    }
}

int are_sets_equal_sorted(int *a, int count_a, int *b, int count_b) {
    if (count_a != count_b) return 0;
    return memcmp(a, b, count_a * sizeof(int)) == 0;
}

void print_progress(long current, long total) {
    const int bar_width = 50;
    float progress = (float)current / total;
    if (progress > 1.0) progress = 1.0;
    if (progress < 0.0) progress = 0.0;
    int filled_width = (int)(bar_width * progress);
    printf("\r[");
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled_width) printf("#"); else printf(" ");
    }
    printf("] %d%% (%ld/%ld)", (int)(progress * 100.0), current, total);
    fflush(stdout); 
}

int add_entry_to_ds(Dataset *ds, int uid, int pid, Interval t) {
    if (ds->count >= ds->capacity) {
        ds->capacity *= 2;
        TupaEntry *new_entries = realloc(ds->entries, ds->capacity * sizeof(TupaEntry));
        if (!new_entries) exit(1);
        ds->entries = new_entries;
    }
    int idx = ds->count;
    ds->entries[idx].uid = uid;
    ds->entries[idx].pid = pid;
    ds->entries[idx].t = t;
    ds->entries[idx].id = idx; 
    ds->entries[idx].covered = 0;
    ds->count++;
    
    int *bucket = ds->user_map[uid];
    int cnt = ds->user_map_counts[uid];
    bucket = realloc(bucket, (cnt + 1) * sizeof(int));
    bucket[cnt] = idx;
    ds->user_map[uid] = bucket;
    ds->user_map_counts[uid]++;
    
    return idx;
}

int subtract_interval(Interval target, Interval sub, Interval *out1, Interval *out2) {
    double inter_start = max_d(target.start, sub.start);
    double inter_end = min_d(target.end, sub.end);
    if (inter_start >= inter_end - EPSILON) { *out1 = target; return 1; }
    int fragments = 0;
    if (inter_start - target.start > EPSILON) {
        out1->start = target.start; out1->end = inter_start; fragments++;
    }
    if (target.end - inter_end > EPSILON) {
        Interval *dest = (fragments == 0) ? out1 : out2;
        dest->start = inter_end; dest->end = target.end; fragments++;
    }
    return fragments;
}

Dataset load_data(const char *upa_path, const char *time_path) {
    Dataset ds;
    ds.count = 0;
    ds.capacity = 8192;
    ds.entries = malloc(ds.capacity * sizeof(TupaEntry));

    char upa_full_path[MAX_PATH];
    char time_full_path[MAX_PATH];
    sprintf(upa_full_path, "%s%s", BASE_DIR, upa_path);
    sprintf(time_full_path, "%s%s", BASE_DIR, time_path);

    printf("Loading data...\n");
    FILE *f_upa = fopen(upa_full_path, "r");
    FILE *f_time = fopen(time_full_path, "r");
    if (!f_upa || !f_time) { printf("CRITICAL ERROR: Could not open input files.\n"); exit(1); }

    char line_upa[MAX_LINE];
    char line_time[MAX_LINE];
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_users = atoi(line_upa); else ds.num_users = 0;
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_perms = atoi(line_upa); else ds.num_perms = 0;
    if (fgets(line_upa, MAX_LINE, f_upa)) { } 
    
    ds.user_map = calloc((ds.num_users + 2), sizeof(int*));
    ds.user_map_counts = calloc((ds.num_users + 2), sizeof(int));

    while (fgets(line_upa, MAX_LINE, f_upa) && fgets(line_time, MAX_LINE, f_time)) {
        int uid, pid;
        if (sscanf(line_upa, "%d %d", &uid, &pid) != 2) continue;
        char *ptr = line_time;
        double s, e;
        int offset;
        while (sscanf(ptr, "%lf %lf%n", &s, &e, &offset) == 2) {
            Interval t = {s, e};
            add_entry_to_ds(&ds, uid, pid, t);
            ptr += offset;
        }
    }
    fclose(f_upa);
    fclose(f_time);
    return ds;
}

// --- POST-PROCESSING: AGGRESSIVE MERGING ---
int merge_roles_aggressive(Role *roles, int num_roles) {
    int merged_count = 0;
    printf("\nPost-processing: Aggressive Merging...\n");

    for(int i=0; i<num_roles; i++) {
        if(roles[i].num_uids > 0) qsort(roles[i].uids, roles[i].num_uids, sizeof(int), compare_ints);
        if(roles[i].num_pids > 0) qsort(roles[i].pids, roles[i].num_pids, sizeof(int), compare_ints);
    }

    int change = 1;
    while (change) {
        change = 0;
        for (int i = 0; i < num_roles; i++) {
            if (!roles[i].valid) continue;

            for (int j = i + 1; j < num_roles; j++) {
                if (!roles[j].valid) continue;

                int time_equal = (fabs(roles[i].t.start - roles[j].t.start) < EPSILON &&
                                  fabs(roles[i].t.end - roles[j].t.end) < EPSILON);
                
                if (time_equal) {
                    if (are_sets_equal_sorted(roles[i].uids, roles[i].num_uids, roles[j].uids, roles[j].num_uids)) {
                        union_sorted_sets(&roles[i], &roles[j], 1); 
                        roles[j].valid = 0; merged_count++; change = 1; continue;
                    }
                    if (are_sets_equal_sorted(roles[i].pids, roles[i].num_pids, roles[j].pids, roles[j].num_pids)) {
                        union_sorted_sets(&roles[i], &roles[j], 0); 
                        roles[j].valid = 0; merged_count++; change = 1; continue;
                    }
                }
            }
        }
    }

    int active_roles = 0;
    for(int i=0; i<num_roles; i++) {
        if(roles[i].valid) active_roles++;
    }
    printf("Merged %d roles. Final count: %d\n", merged_count, active_roles);
    return active_roles;
}

/* ============================
   Replaced logic: implement Algorithm 1 (TRMP-MVC greedy)
   Note: only this function was changed per your instruction.
   ============================ */
void solve_cotrapmp(Dataset *ds, double theta, Role **roles_out, int *num_roles_out) {
    int roles_capacity = 1024;
    Role *roles = malloc(roles_capacity * sizeof(Role));
    int num_roles = 0;

    long initial_total_intervals = 0;
    for (int i = 0; i < ds->count; i++) initial_total_intervals++;

    printf("\nStarting Role Mining (Algorithm 1 - TRMP-MVC greedy)...\n");
    print_progress(0, initial_total_intervals);

    /* helper arrays */
    int *user_checked = calloc(ds->num_users + 2, sizeof(int));
    int *perm_seen = NULL; int perm_seen_cap = 0;
    int *candidate_pids = NULL; int candidate_pids_cap = 0;
    int *candidate_uids = NULL; int candidate_uids_cap = 0;
    int *temp_indices = NULL; int temp_indices_cap = 0;

    long progress_count = 0;
    int iteration = 0;
    const int MAX_ITER = 2000000;

    /* Keep iterating until all entries are fully covered (i.e., no entries left uncovered) */
    while (1) {
        iteration++;
        if (iteration > MAX_ITER) { fprintf(stderr, "\nWarning: reached max iterations\n"); break; }

        /* Check if any uncovered entries remain */
        int any_uncovered = 0;
        for (int i = 0; i < ds->count; i++) {
            if (!ds->entries[i].covered) { any_uncovered = 1; break; }
        }
        if (!any_uncovered) break;

        /* Reset best candidate */
        double best_area = -1.0;
        int *best_U = NULL; int best_U_ct = 0; int best_U_cap = 0;
        int *best_P = NULL; int best_P_ct = 0; int best_P_cap = 0;
        Interval best_T = {0.0, 0.0};
        int *best_covered_indices = NULL; int best_covered_ct = 0; int best_covered_cap = 0;

        /* For each user u that has uncovered entries (seed user) */
        for (int u = 1; u <= ds->num_users; u++) {
            /* check if user has any uncovered entries */
            int ucount = ds->user_map_counts[u];
            if (ucount == 0) continue;
            int user_has_uncovered = 0;
            for (int k = 0; k < ucount; k++) {
                int idx = ds->user_map[u][k];
                if (idx < ds->count && !ds->entries[idx].covered) { user_has_uncovered = 1; break; }
            }
            if (!user_has_uncovered) continue;

            /* For each uncovered (u,p,t) of this user use it as a seed to build a many-valued concept */
            for (int k = 0; k < ucount; k++) {
                int seed_idx = ds->user_map[u][k];
                if (seed_idx >= ds->count) continue;
                TupaEntry *seed_e = &ds->entries[seed_idx];
                if (seed_e->covered) continue;

                /* Candidate permission set P: include all permissions p' of user u whose temporal similarity with seed >= theta.
                   We use temporal σ = (intersection duration) / (union duration) for two intervals. */
                candidate_pids = NULL; candidate_pids_cap = 0;
                int candidate_p_ct = 0;

                for (int kk = 0; kk < ucount; kk++) {
                    int ei = ds->user_map[u][kk];
                    if (ei >= ds->count) continue;
                    if (ds->entries[ei].covered) continue;
                    TupaEntry *e2 = &ds->entries[ei];
                    /* compute σ between seed_e->t and e2->t */
                    double inter_s = max_d(seed_e->t.start, e2->t.start);
                    double inter_e = min_d(seed_e->t.end, e2->t.end);
                    double inter = (inter_e > inter_s) ? (inter_e - inter_s) : 0.0;
                    double uni_s = min_d(seed_e->t.start, e2->t.start);
                    double uni_e = max_d(seed_e->t.end, e2->t.end);
                    double uni = (uni_e > uni_s) ? (uni_e - uni_s) : 0.0;
                    double sigma = (uni <= EPSILON) ? 1.0 : (inter / uni);
                    if (sigma + 1e-12 >= theta) {
                        add_unique(&candidate_pids, &candidate_p_ct, &candidate_pids_cap, e2->pid);
                    }
                }

                if (candidate_p_ct == 0) {
                    if (candidate_pids) free(candidate_pids);
                    candidate_pids = NULL;
                    continue;
                }

                /* Build T as the bounding interval covering all selected perms for the seed user */
                double minStart = 1e308, maxEnd = -1e308;
                for (int pp = 0; pp < candidate_p_ct; pp++) {
                    int pid = candidate_pids[pp];
                    /* find entry for (u,pid) among user's entries and consider uncovered interval */
                    for (int kk = 0; kk < ucount; kk++) {
                        int ei = ds->user_map[u][kk];
                        if (ei >= ds->count) continue;
                        TupaEntry *te = &ds->entries[ei];
                        if (te->pid == pid && !te->covered) {
                            if (te->t.start < minStart) minStart = te->t.start;
                            if (te->t.end > maxEnd) maxEnd = te->t.end;
                            break;
                        }
                    }
                }
                if (minStart > maxEnd) {
                    free(candidate_pids);
                    candidate_pids = NULL;
                    continue;
                }
                Interval candT = { minStart, maxEnd };
                double T_duration = duration(candT);
                if (T_duration <= EPSILON) {
                    free(candidate_pids);
                    candidate_pids = NULL;
                    continue;
                }

                /* Find candidate users U: include any user u' such that for every permission p in P, σ(T(u',p), candT) >= theta. */
                candidate_uids = NULL; candidate_uids_cap = 0;
                int candidate_u_ct = 0;
                for (int uu = 1; uu <= ds->num_users; uu++) {
                    int uu_count = ds->user_map_counts[uu];
                    if (uu_count == 0) continue;
                    int ok = 1;
                    for (int pp = 0; pp < candidate_p_ct; pp++) {
                        int pneeded = candidate_pids[pp];
                        int found_p = 0;
                        for (int kk = 0; kk < uu_count; kk++) {
                            int ei = ds->user_map[uu][kk];
                            if (ei >= ds->count) continue;
                            if (ds->entries[ei].pid != pneeded) continue;
                            if (ds->entries[ei].covered) continue;
                            double inter_s = max_d(ds->entries[ei].t.start, candT.start);
                            double inter_e = min_d(ds->entries[ei].t.end, candT.end);
                            double inter = (inter_e > inter_s) ? (inter_e - inter_s) : 0.0;
                            double uni_s = min_d(ds->entries[ei].t.start, candT.start);
                            double uni_e = max_d(ds->entries[ei].t.end, candT.end);
                            double uni = (uni_e > uni_s) ? (uni_e - uni_s) : 0.0;
                            double sigma = (uni <= EPSILON) ? 1.0 : (inter / uni);
                            if (sigma + 1e-12 >= theta) { found_p = 1; break; }
                        }
                        if (!found_p) { ok = 0; break; }
                    }
                    if (ok) add_unique(&candidate_uids, &candidate_u_ct, &candidate_uids_cap, uu);
                }


                // ... (This is roughly line 366 in your provided code)
                if (candidate_u_ct == 0) { free(candidate_pids); candidate_pids = NULL; if (candidate_uids) free(candidate_uids); candidate_uids = NULL; continue; }

                // ================== START OF FIX ==================
                // CRITICAL FIX: Shrink candT to the INTERSECTION of all selected users.
                // This prevents assigning a role to a user for a time period they don't actually have.
                
                double intersect_start = candT.start;
                double intersect_end = candT.end;
                int intersection_valid = 1;

                for (int iu = 0; iu < candidate_u_ct; iu++) {
                    int uu = candidate_uids[iu];
                    int uu_count = ds->user_map_counts[uu];
                    
                    for (int pp = 0; pp < candidate_p_ct; pp++) {
                        int pneeded = candidate_pids[pp];
                        int found_overlap_entry = 0;
                        
                        // Find the specific entry for this user/perm that overlaps our current window
                        for (int kk = 0; kk < uu_count; kk++) {
                            int ei = ds->user_map[uu][kk];
                            if (ei >= ds->count) continue;
                            if (ds->entries[ei].pid != pneeded) continue;
                            if (ds->entries[ei].covered) continue;
                            
                            double s = max_d(ds->entries[ei].t.start, intersect_start);
                            double e = min_d(ds->entries[ei].t.end, intersect_end);
                            
                            // If valid overlap found, shrink the intersection window
                            if (e > s + EPSILON) {
                                intersect_start = s;
                                intersect_end = e;
                                found_overlap_entry = 1;
                                break; 
                            }
                        }
                        // If a user in the list doesn't actually overlap the window (due to shrinking), the set is invalid
                        if (!found_overlap_entry) { intersection_valid = 0; break; }
                    }
                    if (!intersection_valid) break;
                }

                // If intersection makes the interval invalid or empty, discard this candidate set
                if (!intersection_valid || (intersect_end - intersect_start) <= EPSILON) {
                    free(candidate_pids); candidate_pids = NULL;
                    if (candidate_uids) free(candidate_uids); candidate_uids = NULL;
                    if (temp_indices) free(temp_indices); temp_indices = NULL;
                    continue;
                }

                // Apply the refined intersection to candT
                candT.start = intersect_start;
                candT.end = intersect_end;
                T_duration = duration(candT);
                // ================== END OF FIX ==================

                /* compute area = |U| * |P| * duration(T) */
                double area = (double)candidate_u_ct * (double)candidate_p_ct * T_duration;

                /* compute which entries would be covered and total covered duration */
                temp_indices = NULL; temp_indices_cap = 0;
                int temp_indices_ct = 0;
                double total_covered_duration = 0.0;

                for (int iu = 0; iu < candidate_u_ct; iu++) {
                    int uu = candidate_uids[iu];
                    int uu_count = ds->user_map_counts[uu];
                    for (int pp = 0; pp < candidate_p_ct; pp++) {
                        int pneeded = candidate_pids[pp];
                        for (int kk = 0; kk < uu_count; kk++) {
                            int ei = ds->user_map[uu][kk];
                            if (ei >= ds->count) continue;
                            TupaEntry *te = &ds->entries[ei];
                            if (te->pid != pneeded) continue;
                            if (te->covered) continue;
                            double inter_s = max_d(te->t.start, candT.start);
                            double inter_e = min_d(te->t.end, candT.end);
                            if (inter_e > inter_s + EPSILON) {
                                add_unique(&temp_indices, &temp_indices_ct, &temp_indices_cap, ei);
                                total_covered_duration += (inter_e - inter_s);
                            }
                        }
                    }
                }

                if (temp_indices_ct == 0) {
                    free(candidate_pids); candidate_pids = NULL;
                    if (candidate_uids) free(candidate_uids); candidate_uids = NULL;
                    if (temp_indices) free(temp_indices); temp_indices = NULL;
                    continue;
                }

                /* Accept candidate if area is better than best_area (tie-breaker: use total_covered_duration) */
                int accept = 0;
                if (area > best_area + 1e-12) accept = 1;
                else if (fabs(area - best_area) <= 1e-12) {
                    double best_cov = 0.0;
                    if (best_covered_indices) {
                        for (int bi = 0; bi < best_covered_ct; bi++) {
                            int bi_idx = best_covered_indices[bi];
                            double inter_s = max_d(ds->entries[bi_idx].t.start, best_T.start);
                            double inter_e = min_d(ds->entries[bi_idx].t.end, best_T.end);
                            if (inter_e > inter_s + EPSILON) best_cov += (inter_e - inter_s);
                        }
                    }
                    if (total_covered_duration > best_cov + 1e-9) accept = 1;
                }

                if (accept) {
                    /* free previous best arrays */
                    if (best_U) free(best_U);
                    if (best_P) free(best_P);
                    if (best_covered_indices) free(best_covered_indices);

                    /* copy candidate to best */
                    best_U = NULL; best_U_ct = 0; best_U_cap = 0;
                    for (int a=0;a<candidate_u_ct;a++) add_unique(&best_U, &best_U_ct, &best_U_cap, candidate_uids[a]);
                    best_P = NULL; best_P_ct = 0; best_P_cap = 0;
                    for (int a=0;a<candidate_p_ct;a++) add_unique(&best_P, &best_P_ct, &best_P_cap, candidate_pids[a]);
                    best_T = candT;
                    best_area = area;

                    best_covered_indices = NULL; best_covered_ct = 0; best_covered_cap = 0;
                    for (int a=0;a<temp_indices_ct;a++) add_unique(&best_covered_indices, &best_covered_ct, &best_covered_cap, temp_indices[a]);
                }

                if (candidate_pids) { free(candidate_pids); candidate_pids = NULL; }
                if (candidate_uids) { free(candidate_uids); candidate_uids = NULL; }
                if (temp_indices) { free(temp_indices); temp_indices = NULL; }
            }
        } /* end for each seed user */

        /* If no meaningful best candidate found -> break (likely leftover noise) */
        if (best_area <= 0.0 || best_covered_ct == 0) {
            if (best_U) free(best_U);
            if (best_P) free(best_P);
            if (best_covered_indices) free(best_covered_indices);
            break;
        }

        /* Create a Role from best concept and apply it -> remove / split covered parts across affected entries */
        if (num_roles >= roles_capacity) {
            roles_capacity *= 2;
            roles = realloc(roles, roles_capacity * sizeof(Role));
            if (!roles) { perror("realloc roles"); exit(1); }
        }

        Role newr;
        newr.uids = best_U; newr.num_uids = best_U_ct; newr.uids_capacity = best_U_cap;
        newr.pids = best_P; newr.num_pids = best_P_ct; newr.pids_capacity = best_P_cap;
        newr.t = best_T;
        newr.temp_covered_indices = best_covered_indices;
        newr.temp_num_covered = best_covered_ct;
        newr.valid = 1;
        newr.split_count = 0;
        newr.overlap_count = best_covered_ct;
        newr.id = num_roles + 1;

        roles[num_roles++] = newr;

        /* Mark and split covered entries */
        for (int bi = 0; bi < newr.temp_num_covered; bi++) {
            int ei = newr.temp_covered_indices[bi];
            if (ei >= ds->count) continue;
            TupaEntry *ent = &ds->entries[ei];
            if (ent->covered) continue;
            /* compute intersection with newr.t */
            double inter_s = max_d(ent->t.start, newr.t.start);
            double inter_e = min_d(ent->t.end, newr.t.end);
            if (inter_e <= inter_s + EPSILON) continue;
            Interval inter = { inter_s, inter_e };

            /* Correct subtraction semantics:
               0 = no overlap
               1 = one fragment remains (out1)
               2 = two fragments remain (out1 and out2)
               3 = fully covered (no fragment remains)
            */
            Interval out1, out2;
            int frags = 0;
            /* no overlap */
            if (inter_e <= ent->t.start + EPSILON || inter_s >= ent->t.end - EPSILON) {
                frags = 0;
            } else {
                /* fully covered */
                if (inter_s <= ent->t.start + EPSILON && inter_e >= ent->t.end - EPSILON) {
                    frags = 3;
                } else {
                    /* partials */
                    int cnt = 0;
                    if (inter_s - ent->t.start > EPSILON) {
                        out1.start = ent->t.start; out1.end = inter_s; cnt++;
                    }
                    if (ent->t.end - inter_e > EPSILON) {
                        if (cnt == 0) {
                            out1.start = inter_e; out1.end = ent->t.end; cnt = 1;
                        } else {
                            out2.start = inter_e; out2.end = ent->t.end; cnt = 2;
                        }
                    }
                    frags = cnt;
                }
            }

            if (frags == 0) {
                /* no overlap - nothing to do */
                continue;
            } else if (frags == 3) {
                /* fully consumed */
                ent->covered = 1;
            } else if (frags == 1) {
                /* single remaining fragment */
                ent->t = out1;
            } else if (frags == 2) {
                /* split: keep out1, create out2 */
                ent->t = out1;
                add_entry_to_ds(ds, ent->uid, ent->pid, out2);
            }
        }

        /* progress print: recompute covered count approx as number of entries marked covered */
        int covered_cnt = 0;
        for (int i=0; i<ds->count; i++) if (ds->entries[i].covered) covered_cnt++;
        print_progress(covered_cnt, initial_total_intervals);

    } /* end greedy loop */

    /* finalize outputs */
    print_progress(initial_total_intervals, initial_total_intervals);
    printf("\nRole mining complete. Total raw roles: %d\n", num_roles);

    free(user_checked);
    if (perm_seen) free(perm_seen);
    if (candidate_pids) free(candidate_pids);
    if (candidate_uids) free(candidate_uids);
    if (temp_indices) free(temp_indices);

    *roles_out = roles;
    *num_roles_out = num_roles;
}
/* ============================
   End of replaced function
   ============================ */

void write_outputs(Dataset *ds, Role *roles, int num_roles) {
    int active_roles = merge_roles_aggressive(roles, num_roles);
    char f_ua[MAX_PATH], f_pa[MAX_PATH], f_reb[MAX_PATH];
    sprintf(f_ua, "%s%s", BASE_DIR, OUTPUT_UA);
    sprintf(f_pa, "%s%s", BASE_DIR, OUTPUT_PA);
    sprintf(f_reb, "%s%s", BASE_DIR, OUTPUT_REB);

    printf("Writing outputs to:\n  %s\n  %s\n  %s\n", f_ua, f_pa, f_reb);
    FILE *ua = fopen(f_ua, "w");
    FILE *pa = fopen(f_pa, "w");
    FILE *reb = fopen(f_reb, "w");
    if(!ua || !pa || !reb) { perror("Error opening output files"); exit(1); }

    fprintf(ua, "%d\n%d\n", ds->num_users, active_roles);
    fprintf(pa, "%d\n%d\n", active_roles, ds->num_perms);
    fprintf(reb, "%d\n", active_roles);

    int current_id = 1;
    int total_pa = 0;
    for(int i=0; i<num_roles; i++) {
        if (!roles[i].valid) continue; 
        for(int j=0; j<roles[i].num_uids; j++) fprintf(ua, "u%d r%d\n", roles[i].uids[j], current_id);
        for(int j=0; j<roles[i].num_pids; j++) {
            fprintf(pa, "p%d r%d\n", roles[i].pids[j], current_id);
            total_pa++;
        }
        fprintf(reb, "r%d %.1f %.1f\n", current_id, roles[i].t.start, roles[i].t.end);
        current_id++;
    }
    fclose(ua); fclose(pa); fclose(reb);
    printf("--- Final Stats ---\n");
    printf("|R| = %d\n", active_roles);
    // Outputting total stats including PA+REB count
    printf("|PA| + |REB| = %d\n", total_pa + active_roles);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: %s <THETA>\n", argv[0]); return 1; }
    double theta = atof(argv[1]);
    Dataset ds = load_data(INPUT_UPA, INPUT_TIME);
    if (ds.count == 0) { printf("Error: Dataset empty.\n"); return 1; }
    printf("Dataset loaded. Entries: %d\n", ds.count);
    Role *roles = NULL;
    int num_roles = 0;
    solve_cotrapmp(&ds, theta, &roles, &num_roles);
    write_outputs(&ds, roles, num_roles);
    free(ds.entries);
    for(int i=0; i<=ds.num_users; i++) if(ds.user_map[i]) free(ds.user_map[i]);
    free(ds.user_map); free(ds.user_map_counts);
    for(int i=0; i<num_roles; i++) { if(roles[i].uids) free(roles[i].uids); if(roles[i].pids) free(roles[i].pids); }
    free(roles);
    return 0;
}