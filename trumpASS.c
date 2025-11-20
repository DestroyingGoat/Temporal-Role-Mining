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
#define INPUT_TIME "TIME_10int_c100_apj.txt" 

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
    
    // For algo use
    int *temp_covered_indices;
    int temp_num_covered;
    int split_count;
    int overlap_count;
    int valid; // For merging (1=valid, 0=merged/deleted)
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

// Simple integer compare for qsort
int compare_ints(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

void add_to_array(int **arr, int *count, int val) {
    int new_capacity = *count + 1;
    int *new_arr = realloc(*arr, new_capacity * sizeof(int));
    if (new_arr == NULL) {
        perror("realloc failed");
        exit(EXIT_FAILURE); 
    }
    *arr = new_arr;
    (*arr)[*count] = val;
    (*count)++;
}

// Helper to check set equality (arrays must be sorted for this to work efficiently)
int are_sets_equal(int *a, int count_a, int *b, int count_b) {
    if (count_a != count_b) return 0;
    // Sort both temporary copies to ensure order doesn't matter
    // (In a real optimized version, we'd keep them sorted, but this is safer)
    int *a_copy = malloc(count_a * sizeof(int));
    int *b_copy = malloc(count_b * sizeof(int));
    memcpy(a_copy, a, count_a * sizeof(int));
    memcpy(b_copy, b, count_b * sizeof(int));
    qsort(a_copy, count_a, sizeof(int), compare_ints);
    qsort(b_copy, count_b, sizeof(int), compare_ints);
    
    int equal = 1;
    for(int i=0; i<count_a; i++) {
        if(a_copy[i] != b_copy[i]) {
            equal = 0;
            break;
        }
    }
    free(a_copy);
    free(b_copy);
    return equal;
}

// Helper to merge array b into array a (union)
void union_sets(int **a, int *count_a, int *b, int count_b) {
    for (int i = 0; i < count_b; i++) {
        int val = b[i];
        int found = 0;
        for (int j = 0; j < *count_a; j++) {
            if ((*a)[j] == val) {
                found = 1;
                break;
            }
        }
        if (!found) {
            add_to_array(a, count_a, val);
        }
    }
}

// Progress Bar
void print_progress(long current, long total) {
    const int bar_width = 50;
    float progress = (float)current / total;
    if (progress > 1.0) progress = 1.0;
    if (progress < 0.0) progress = 0.0;
    int filled_width = (int)(bar_width * progress);
    printf("\r[");
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled_width) printf("#");
        else printf(" ");
    }
    printf("] %d%% (%ld/%ld)", (int)(progress * 100.0), current, total);
    fflush(stdout); 
}

int add_entry_to_ds(Dataset *ds, int uid, int pid, Interval t) {
    if (ds->count >= ds->capacity) {
        ds->capacity *= 2;
        TupaEntry *new_entries = realloc(ds->entries, ds->capacity * sizeof(TupaEntry));
        if (new_entries == NULL) exit(1);
        ds->entries = new_entries;
    }
    int idx = ds->count;
    ds->entries[idx].uid = uid;
    ds->entries[idx].pid = pid;
    ds->entries[idx].t = t;
    ds->entries[idx].id = idx; 
    ds->entries[idx].covered = 0;
    ds->count++;
    if (uid <= ds->num_users) {
        add_to_array(&ds->user_map[uid], &ds->user_map_counts[uid], idx);
    }
    return idx;
}

int subtract_interval(Interval target, Interval sub, Interval *out1, Interval *out2) {
    double inter_start = max_d(target.start, sub.start);
    double inter_end = min_d(target.end, sub.end);
    if (inter_start >= inter_end - EPSILON) {
        *out1 = target;
        return 1;
    }
    int fragments = 0;
    if (inter_start - target.start > EPSILON) {
        out1->start = target.start;
        out1->end = inter_start;
        fragments++;
    }
    if (target.end - inter_end > EPSILON) {
        Interval *dest = (fragments == 0) ? out1 : out2;
        dest->start = inter_end;
        dest->end = target.end;
        fragments++;
    }
    return fragments;
}

Dataset load_data(const char *upa_path, const char *time_path) {
    Dataset ds;
    ds.count = 0;
    ds.capacity = 2000;
    ds.entries = malloc(ds.capacity * sizeof(TupaEntry));

    char upa_full_path[MAX_PATH];
    char time_full_path[MAX_PATH];
    sprintf(upa_full_path, "%s%s", BASE_DIR, upa_path);
    sprintf(time_full_path, "%s%s", BASE_DIR, time_path);

    printf("Attempting to load:\n  1. %s\n  2. %s\n", upa_full_path, time_full_path);
    FILE *f_upa = fopen(upa_full_path, "r");
    FILE *f_time = fopen(time_full_path, "r");

    if (!f_upa || !f_time) {
        printf("CRITICAL ERROR: Could not open input files.\n");
        exit(1);
    }

    char line_upa[MAX_LINE];
    char line_time[MAX_LINE];

    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_users = atoi(line_upa); else ds.num_users = 0;
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_perms = atoi(line_upa); else ds.num_perms = 0;
    if (fgets(line_upa, MAX_LINE, f_upa)) { } 

    ds.user_map = malloc((ds.num_users + 2) * sizeof(int*));
    ds.user_map_counts = calloc((ds.num_users + 2), sizeof(int));
    for(int i=0; i<=ds.num_users; i++) ds.user_map[i] = NULL;

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

// --- POST-PROCESSING: MERGING CONCEPTS ---
// Implements the "merge concepts of C" step from the paper (Lines 18/22)
int merge_roles(Role *roles, int num_roles) {
    int merged_count = 0;
    int active_roles = 0;

    printf("\nPost-processing: Merging compatible roles...\n");

    // Iterate until no more merges can be performed (convergence)
    int change = 1;
    while (change) {
        change = 0;
        for (int i = 0; i < num_roles; i++) {
            if (!roles[i].valid) continue;

            for (int j = i + 1; j < num_roles; j++) {
                if (!roles[j].valid) continue;

                // Rule 1: Merge TIME (Same Users, Same Perms, Overlapping/Consecutive Time)
                // Not fully implemented for consecutive logic for simplicity, checking overlap/adjacent
                // Actually, paper allows "Union of T" if U_i=U_j and P_i=P_j.
                // But our Roles have SINGLE intervals. If we merge, we might create multi-interval roles.
                // The current struct Role has "Interval t". 
                // If we merge time, it effectively means we might need to allow multiple intervals per role OR 
                // simply combine them if they touch. 
                // NOTE: The standard TRBAC output format (REB) usually allows multiple intervals per role ID.
                // However, the structure here has 1 interval. 
                // STRATEGY: We will skip Time merging if it results in non-contiguous intervals for this implementation context
                // unless we change the Role struct to support list of intervals.
                // Instead, let's focus on Rule 2 and 3 which reduce |R| count significantly.
                
                // Rule 2: Merge PERMISSIONS (Same Users, Same Time)
                if (fabs(roles[i].t.start - roles[j].t.start) < EPSILON &&
                    fabs(roles[i].t.end - roles[j].t.end) < EPSILON &&
                    are_sets_equal(roles[i].uids, roles[i].num_uids, roles[j].uids, roles[j].num_uids)) {
                    
                    // Merge Perms of J into I
                    union_sets(&roles[i].pids, &roles[i].num_pids, roles[j].pids, roles[j].num_pids);
                    roles[j].valid = 0; // Mark J as merged
                    merged_count++;
                    change = 1;
                    continue; 
                }

                // Rule 3: Merge USERS (Same Perms, Same Time)
                if (fabs(roles[i].t.start - roles[j].t.start) < EPSILON &&
                    fabs(roles[i].t.end - roles[j].t.end) < EPSILON &&
                    are_sets_equal(roles[i].pids, roles[i].num_pids, roles[j].pids, roles[j].num_pids)) {
                    
                    // Merge Users of J into I
                    union_sets(&roles[i].uids, &roles[i].num_uids, roles[j].uids, roles[j].num_uids);
                    roles[j].valid = 0; // Mark J as merged
                    merged_count++;
                    change = 1;
                    continue;
                }
            }
        }
    }

    // Count valid roles remaining
    for(int i=0; i<num_roles; i++) {
        if(roles[i].valid) active_roles++;
    }
    printf("Merged %d redundant role fragments. Final role count: %d\n", merged_count, active_roles);
    return active_roles;
}


// --- Algorithm 2 Core ---

void solve_cotrapmp(Dataset *ds, double theta, Role **roles_out, int *num_roles_out) {
    int num_roles = 0;
    Role *roles = NULL;
    int *checked_users = calloc(ds->num_users + 2, sizeof(int)); 
    
    long initial_entries_to_cover = ds->count;
    long covered_count = 0;

    printf("\nStarting Role Mining (Algorithm 2 - CO-TRAPMP)...\n");
    print_progress(0, initial_entries_to_cover);

    int found_any = 1;

    while (found_any) {
        found_any = 0;
        double max_coverage = -EPSILON; 
        int max_overlap_split = 2147483647; 

        Role best_role = {0};
        int found_pass = 0;

        memset(checked_users, 0, (ds->num_users + 2) * sizeof(int));

        int initial_count = ds->count;
        for (int i = 0; i < initial_count; i++) {
            if (i >= ds->count || ds->entries[i].covered) continue;

            int uid = ds->entries[i].uid;
            if (checked_users[uid]) continue; 
            checked_users[uid] = 1;

            Interval seed_t = ds->entries[i].t;

            int *cand_pids = NULL; 
            int num_cp = 0;
            
            int *u_idx = ds->user_map[uid];
            int u_cnt = ds->user_map_counts[uid];

            for(int k=0; k<u_cnt; k++) {
                int idx = u_idx[k];
                if (idx >= ds->count) continue;
                if (ds->entries[idx].covered) continue; // Algo 2: Only uncovered perms for seed

                if (is_subset(ds->entries[idx].t, seed_t)) {
                    int pid = ds->entries[idx].pid;
                    int exists = 0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == pid) { exists=1; break; }
                    if(!exists) add_to_array(&cand_pids, &num_cp, pid);
                }
            }

            if(num_cp == 0) { free(cand_pids); continue; }

            int *cand_uids = NULL;
            int num_cu = 0;

            for (int u = 1; u <= ds->num_users; u++) {
                int has_all = 1;
                for (int p_idx = 0; p_idx < num_cp; p_idx++) {
                    int needed_p = cand_pids[p_idx];
                    int p_ok = 0;
                    int *chk_idx = ds->user_map[u];
                    int chk_cnt = ds->user_map_counts[u];

                    for(int c=0; c<chk_cnt; c++) {
                        int idx = chk_idx[c];
                        if (idx >= ds->count) continue;
                        // Algo 2: Candidate users CAN be covered (overlap penalty handled later)
                        if(ds->entries[idx].pid == needed_p) {
                            if (is_subset(ds->entries[idx].t, seed_t)) {
                                p_ok = 1; break;
                            }
                        }
                    }
                    if (!p_ok) { has_all = 0; break; }
                }
                if (has_all) add_to_array(&cand_uids, &num_cu, u);
            }

            double current_cov = 0.0;
            int current_splits = 0;
            int current_overlaps = 0;
            int *covered_indices = NULL;
            int num_covered_indices = 0;
            int *index_map_check = calloc(ds->count + 2, sizeof(int));
            
            for(int u_i=0; u_i<num_cu; u_i++) {
                int u = cand_uids[u_i];
                int *chk_idx = ds->user_map[u];
                int chk_cnt = ds->user_map_counts[u];
                
                for(int c=0; c<chk_cnt; c++) {
                    int idx = chk_idx[c];
                    if (idx >= ds->count || index_map_check[idx]) continue;

                    TupaEntry *e = &ds->entries[idx];
                    int is_in_perm = 0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == e->pid) { is_in_perm=1; break; }
                    
                    if(is_in_perm && is_subset(e->t, seed_t)) {
                        if (e->covered) {
                            current_overlaps++;
                        } else {
                            current_cov += duration(seed_t);
                            Interval r1, r2;
                            int frags = subtract_interval(e->t, seed_t, &r1, &r2);
                            if (frags > 0) current_splits++;
                            add_to_array(&covered_indices, &num_covered_indices, idx);
                        }
                        index_map_check[idx] = 1;
                    }
                }
            }
            free(index_map_check);

            // Algo 2 Selection Logic
            int current_cost = current_splits + current_overlaps;
            int is_better = 0;

            if (current_cov > max_coverage + EPSILON) {
                is_better = 1; 
            } else if (fabs(current_cov - max_coverage) < EPSILON) {
                if (current_cost < max_overlap_split) {
                    is_better = 1;
                }
            }

            if (is_better) { 
                if (found_pass) { 
                    free(best_role.uids); free(best_role.pids); free(best_role.temp_covered_indices); 
                }
                best_role.uids = cand_uids;
                best_role.num_uids = num_cu;
                best_role.pids = cand_pids;
                best_role.num_pids = num_cp;
                best_role.t = seed_t;
                best_role.temp_covered_indices = covered_indices;
                best_role.temp_num_covered = num_covered_indices;
                best_role.valid = 1; // Mark as initially valid
                
                max_coverage = current_cov;
                max_overlap_split = current_cost;
                found_pass = 1;
            } else {
                free(cand_uids);
                free(cand_pids);
                free(covered_indices);
            }
        }

        if (found_pass && max_coverage > EPSILON) {
            found_any = 1;
            int *indices_to_process = best_role.temp_covered_indices;
            int count_to_process = best_role.temp_num_covered;

            best_role.id = ++num_roles;
            best_role.temp_covered_indices = NULL; 

            roles = realloc(roles, num_roles * sizeof(Role));
            if (roles == NULL) exit(1);
            roles[num_roles-1] = best_role;

            int current_ds_count = ds->count; 
            for(int i=0; i<count_to_process; i++) {
                int idx = indices_to_process[i];
                if (idx >= current_ds_count) continue; 
                if (ds->entries[idx].covered) continue;

                TupaEntry *e = &ds->entries[idx];
                Interval r1, r2;
                int frags = subtract_interval(e->t, best_role.t, &r1, &r2);
                
                if (frags == 0) {
                    e->covered = 1;
                    if (idx < initial_entries_to_cover) covered_count++;
                } else if (frags == 1) {
                    e->t = r1;
                } else if (frags == 2) {
                    e->t = r1; 
                    add_entry_to_ds(ds, e->uid, e->pid, r2); 
                }
            }
            free(indices_to_process);
            print_progress(covered_count, initial_entries_to_cover);

        } else {
            if (found_pass) { free(best_role.uids); free(best_role.pids); free(best_role.temp_covered_indices); }
            break;
        }
    }
    
    print_progress(initial_entries_to_cover, initial_entries_to_cover);
    printf("\nRole mining complete. Total raw roles: %d", num_roles);
    
    free(checked_users);
    
    // Perform Merging to reduce role count
    *roles_out = roles;
    *num_roles_out = num_roles;
}

// --- Write Output ---

void write_outputs(Dataset *ds, Role *roles, int num_roles) {
    // Run merge logic just before writing
    int active_roles = merge_roles(roles, num_roles);

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

    // Renumber roles sequentially during output
    int current_id = 1;
    for(int i=0; i<num_roles; i++) {
        if (!roles[i].valid) continue; // Skip merged roles

        // Update ID for output
        int old_id = roles[i].id; // Keep old for debugging if needed, but spec implies sequential IDs
        
        // Write UA
        for(int j=0; j<roles[i].num_uids; j++) {
            fprintf(ua, "u%d r%d\n", roles[i].uids[j], current_id);
        }
        
        // Write PA
        for(int j=0; j<roles[i].num_pids; j++) {
            fprintf(pa, "p%d r%d\n", roles[i].pids[j], current_id);
        }
        
        // Write REB
        fprintf(reb, "r%d %.1f %.1f\n", current_id, roles[i].t.start, roles[i].t.end);
        
        current_id++;
    }

    fclose(ua);
    fclose(pa);
    fclose(reb);
    
    printf("--- Final Stats ---\n");
    printf("|R| = %d\n", active_roles);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <THETA>\n", argv[0]);
        return 1;
    }
    double theta = atof(argv[1]);
    
    Dataset ds = load_data(INPUT_UPA, INPUT_TIME);
    
    if (ds.count == 0) {
        printf("Error: Dataset empty.\n");
        return 1;
    }
    printf("Dataset loaded. Entries: %d\n", ds.count);
    
    Role *roles = NULL;
    int num_roles = 0;
    
    solve_cotrapmp(&ds, theta, &roles, &num_roles);
    write_outputs(&ds, roles, num_roles);
    
    // Cleanup
    free(ds.entries);
    for(int i=0; i<=ds.num_users; i++) if(ds.user_map[i]) free(ds.user_map[i]);
    free(ds.user_map);
    free(ds.user_map_counts);
    for(int i=0; i<num_roles; i++) {
        if(roles[i].uids) free(roles[i].uids);
        if(roles[i].pids) free(roles[i].pids);
    }
    free(roles);
    
    return 0;
}