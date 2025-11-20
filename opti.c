#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- Configuration ---
#define MAX_LINE 8192
#define MAX_PATH 1024
#define EPSILON 1e-6 
#define BASE_DIR "./" 
#define INPUT_UPA "fire1.txt"
#define INPUT_TIME "TIME_10int_c25_o75_fire1.txt" 

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

// --- ALGORITHM 1: TRMP-MVC Implementation ---
void solve_trmp_algo1(Dataset *ds, double theta, Role **roles_out, int *num_roles_out) {
    int num_roles = 0;
    int roles_capacity = 1000;
    Role *roles = malloc(roles_capacity * sizeof(Role));
    
    int *checked_users = calloc(ds->num_users + 2, sizeof(int)); 
    long initial_entries_to_cover = ds->count;
    long covered_count = 0;

    printf("\nStarting Role Mining (Algorithm 1 - TRMP-MVC - Max Coverage)...\n");
    print_progress(0, initial_entries_to_cover);

    // Candidate Buffers
    int *cand_pids = NULL; int cp_cap = 0;
    int *cand_uids = NULL; int cu_cap = 0;
    int *covered_idx = NULL; int ci_cap = 0;
    int *idx_map = calloc(ds->capacity + 5000, sizeof(int)); 

    int found_any = 1;
    while (found_any) {
        found_any = 0;
        double max_coverage = -EPSILON; 
        
        Role best_role = {0};
        int found_pass = 0;

        memset(checked_users, 0, (ds->num_users + 1) * sizeof(int));

        int initial_count = ds->count;
        for (int i = 0; i < initial_count; i++) {
            if (i >= ds->count || ds->entries[i].covered) continue;
            int uid = ds->entries[i].uid;
            
            // Ensure we check a user only once per iteration as a seed
            if (checked_users[uid]) continue; 
            checked_users[uid] = 1;
            
            Interval seed_t = ds->entries[i].t;

            // 1. Determine set of permissions P of u (uncovered/partial)
            int num_cp = 0;
            int *u_idx = ds->user_map[uid];
            int u_cnt = ds->user_map_counts[uid];

            for(int k=0; k<u_cnt; k++) {
                int idx = u_idx[k];
                if (idx >= ds->count) continue;
                if (ds->entries[idx].covered) continue; // Must be uncovered/partial for seed
                if (is_subset(ds->entries[idx].t, seed_t)) {
                    int exists=0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == ds->entries[idx].pid) { exists=1; break; }
                    if(!exists) add_to_array(&cand_pids, &num_cp, &cp_cap, ds->entries[idx].pid);
                }
            }
            if(num_cp == 0) continue;

            // 2. Determine set of users U_l that have permissions P for time seed_t
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
                        if(ds->entries[idx].pid == needed_p) {
                            if (is_subset(ds->entries[idx].t, seed_t)) { p_ok = 1; break; }
                        }
                    }
                    if (!p_ok) { has_all = 0; break; }
                }
                if (has_all) add_to_array(&cand_uids, &num_cu, &cu_cap, u);
            }
            if (num_cu == 0) continue;

            // 3. ALGO 1 STEP: EXPAND PERMISSIONS (Find Maximal P_l)
            // Determine set of permissions P_l such that ALL users in U_l have them for seed_t
            // We start with the permissions of the first user in the list and filter.
            // Note: P_l >= P (cand_pids currently holds P)
            
            int first_u = cand_uids[0];
            int *fu_idx = ds->user_map[first_u];
            int fu_cnt = ds->user_map_counts[first_u];
            
            // We need to scan all permissions of the first user to find *more* potential shared permissions
            for(int k=0; k<fu_cnt; k++) {
                int idx = fu_idx[k];
                if (idx >= ds->count) continue;
                if (is_subset(ds->entries[idx].t, seed_t)) {
                    int candidate_p = ds->entries[idx].pid;
                    
                    // Check if this permission is already in cand_pids (Step 1 set)
                    int already_in = 0;
                    for(int p=0; p<num_cp; p++) { if(cand_pids[p] == candidate_p) { already_in=1; break; } }
                    if(already_in) continue;

                    // Check if ALL other users have this permission for seed_t
                    int all_have = 1;
                    for(int u_i=1; u_i<num_cu; u_i++) { // Start from 1, since 0 is first_u
                        int u_chk = cand_uids[u_i];
                        int found_p = 0;
                        int *chk_idx = ds->user_map[u_chk];
                        int chk_cnt = ds->user_map_counts[u_chk];
                        for(int c=0; c<chk_cnt; c++) {
                            int c_idx = chk_idx[c];
                            if(ds->entries[c_idx].pid == candidate_p && is_subset(ds->entries[c_idx].t, seed_t)) {
                                found_p = 1; break;
                            }
                        }
                        if(!found_p) { all_have = 0; break; }
                    }
                    
                    if(all_have) {
                        add_to_array(&cand_pids, &num_cp, &cp_cap, candidate_p);
                    }
                }
            }

            // 4. Calculate Area of Coverage (Only for uncovered/partial parts)
            double current_cov = 0.0;
            int num_ci = 0;
            
            for(int u_i=0; u_i<num_cu; u_i++) {
                int u = cand_uids[u_i];
                int *chk_idx = ds->user_map[u];
                int chk_cnt = ds->user_map_counts[u];
                for(int c=0; c<chk_cnt; c++) {
                    int idx = chk_idx[c];
                    if (idx >= ds->count || idx_map[idx]) continue;
                    TupaEntry *e = &ds->entries[idx];
                    
                    int is_in_perm = 0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == e->pid) { is_in_perm=1; break; }
                    
                    if(is_in_perm && is_subset(e->t, seed_t)) {
                        // Only contribute to score if not fully covered
                        if (!e->covered) {
                            current_cov += duration(seed_t);
                            add_to_array(&covered_idx, &num_ci, &ci_cap, idx);
                        }
                        idx_map[idx] = 1;
                    }
                }
            }
            
            // Reset map
            for(int u_i=0; u_i<num_cu; u_i++) {
                int u = cand_uids[u_i];
                int *chk_idx = ds->user_map[u];
                int chk_cnt = ds->user_map_counts[u];
                for(int c=0; c<chk_cnt; c++) idx_map[chk_idx[c]] = 0;
            }

            // 5. Selection Heuristic: Algorithm 1 purely selects based on Max Coverage
            if (current_cov > max_coverage + EPSILON) { 
                if (found_pass) { 
                    if(best_role.uids) free(best_role.uids);
                    if(best_role.pids) free(best_role.pids);
                    if(best_role.temp_covered_indices) free(best_role.temp_covered_indices);
                }
                best_role.uids = malloc(num_cu * sizeof(int));
                memcpy(best_role.uids, cand_uids, num_cu * sizeof(int));
                best_role.num_uids = num_cu;
                
                best_role.pids = malloc(num_cp * sizeof(int));
                memcpy(best_role.pids, cand_pids, num_cp * sizeof(int));
                best_role.num_pids = num_cp;
                
                best_role.t = seed_t;
                
                best_role.temp_covered_indices = malloc(num_ci * sizeof(int));
                memcpy(best_role.temp_covered_indices, covered_idx, num_ci * sizeof(int));
                best_role.temp_num_covered = num_ci;
                
                best_role.valid = 1;
                best_role.uids_capacity = num_cu; 
                best_role.pids_capacity = num_cp;

                max_coverage = current_cov;
                found_pass = 1;
                found_any = 1; 
            }
        }

        if (found_pass && max_coverage > EPSILON) {
            if (num_roles >= roles_capacity) {
                roles_capacity *= 2;
                roles = realloc(roles, roles_capacity * sizeof(Role));
            }
            best_role.id = num_roles + 1;
            roles[num_roles++] = best_role; 

            int *indices = best_role.temp_covered_indices;
            int count = best_role.temp_num_covered;

            int current_ds_count = ds->count; 
            if (ds->capacity + 2000 > (ds->capacity + 5000)) { 
                free(idx_map);
                idx_map = calloc(ds->capacity + 5000, sizeof(int));
            }

            for(int i=0; i<count; i++) {
                int idx = indices[i];
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
            free(roles[num_roles-1].temp_covered_indices);
            roles[num_roles-1].temp_covered_indices = NULL;

            print_progress(covered_count, initial_entries_to_cover);
        } else {
            break;
        }
    }
    
    print_progress(initial_entries_to_cover, initial_entries_to_cover);
    printf("\nRole mining complete. Total raw roles: %d", num_roles);
    
    free(checked_users);
    free(cand_pids);
    free(cand_uids);
    free(covered_idx);
    free(idx_map);
    
    *roles_out = roles;
    *num_roles_out = num_roles;
}

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
    // Call Algorithm 1 implementation
    solve_trmp_algo1(&ds, theta, &roles, &num_roles);
    write_outputs(&ds, roles, num_roles);
    free(ds.entries);
    for(int i=0; i<=ds.num_users; i++) if(ds.user_map[i]) free(ds.user_map[i]);
    free(ds.user_map); free(ds.user_map_counts);
    for(int i=0; i<num_roles; i++) { if(roles[i].uids) free(roles[i].uids); if(roles[i].pids) free(roles[i].pids); }
    free(roles);
    return 0;
}