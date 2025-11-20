#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

// --- Configuration ---
#define MAX_LINE 8192
#define MAX_PATH 1024

// 1. TWEAK: Dust filter set to 1e-5 to ignore floating point noise 
//    common in "Contained" vs "Overlapping" boundary math.
#define EPSILON 1e-9 
#define MIN_DURATION 1e-5 

// AUTOMATIC FOLDER CONFIGURATION
#define DATA_FOLDER "datasets/" 

// Output filenames
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
    
    int num_users_header;
    int num_perms_header;
    int max_uid_found;
    int max_pid_found;
    
    int **user_map; 
    int *user_map_counts;
    int user_map_size;    

    int **pid_map;          
    int *pid_map_counts;
    int *pid_map_caps;
    int pid_map_size;
} Dataset;

typedef struct {
    int *uids;
    int num_uids;
    int *pids;
    int num_pids;
    Interval t;
    int id;
    
    int uids_capacity;
    int pids_capacity;
    int valid; 
} Role;

typedef struct {
    int uid;
    int pid;
    Interval t;
} PendingEntry;

// --- Helper Functions ---

double max_d(double a, double b) { return (a > b) ? a : b; }
double min_d(double a, double b) { return (a < b) ? a : b; }

double duration(Interval iv) {
    double d = iv.end - iv.start;
    return (d < 0) ? 0.0 : d;
}

// Robust Jaccard
double calc_jaccard(Interval a, Interval b) {
    double inter_s = max_d(a.start, b.start);
    double inter_e = min_d(a.end, b.end);
    
    double len_inter = 0.0;
    if (inter_e > inter_s + EPSILON) {
        len_inter = inter_e - inter_s;
    } else {
        return 0.0; 
    }
    
    double len_a = duration(a);
    double len_b = duration(b);
    double len_union = len_a + len_b - len_inter;
    
    if (len_union <= EPSILON) return 1.0;
    return len_inter / len_union;
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
    if (total <= 0) total = 1;
    float progress = (float)current / total;
    if (progress > 1.0) progress = 1.0;
    int filled_width = (int)(bar_width * progress);
    printf("\r[");
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled_width) printf("#"); else printf(" ");
    }
    printf("] %d%% (%ld/%ld)", (int)(progress * 100.0), current, total);
    fflush(stdout); 
}

// --- SAFE LOADING ---

void ensure_user_map_size(Dataset *ds, int needed_uid) {
    if (needed_uid >= ds->user_map_size) {
        int new_size = (needed_uid + 1) * 2;
        ds->user_map = realloc(ds->user_map, new_size * sizeof(int*));
        ds->user_map_counts = realloc(ds->user_map_counts, new_size * sizeof(int));
        for (int i = ds->user_map_size; i < new_size; i++) {
            ds->user_map[i] = NULL;
            ds->user_map_counts[i] = 0;
        }
        ds->user_map_size = new_size;
    }
    if (needed_uid > ds->max_uid_found) ds->max_uid_found = needed_uid;
}

void ensure_pid_map_size(Dataset *ds, int needed_pid) {
    if (needed_pid >= ds->pid_map_size) {
        int new_size = (needed_pid + 1) * 2; 
        ds->pid_map = realloc(ds->pid_map, new_size * sizeof(int*));
        ds->pid_map_counts = realloc(ds->pid_map_counts, new_size * sizeof(int));
        ds->pid_map_caps = realloc(ds->pid_map_caps, new_size * sizeof(int));
        for (int i = ds->pid_map_size; i < new_size; i++) {
            ds->pid_map[i] = NULL;
            ds->pid_map_counts[i] = 0;
            ds->pid_map_caps[i] = 0;
        }
        ds->pid_map_size = new_size;
    }
    if (needed_pid > ds->max_pid_found) ds->max_pid_found = needed_pid;
}

int add_entry_to_ds(Dataset *ds, int uid, int pid, Interval t) {
    if (ds->count >= ds->capacity) {
        ds->capacity = (ds->capacity == 0) ? 8192 : ds->capacity * 2;
        TupaEntry *new_entries = realloc(ds->entries, ds->capacity * sizeof(TupaEntry));
        if (!new_entries) { perror("Realloc Entries Failed"); exit(1); }
        ds->entries = new_entries;
    }
    
    ensure_user_map_size(ds, uid);
    ensure_pid_map_size(ds, pid);

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

    add_to_array(&ds->pid_map[pid], &ds->pid_map_counts[pid], &ds->pid_map_caps[pid], idx);
    
    return idx;
}

int subtract_interval(Interval target, Interval sub, Interval *out1, Interval *out2) {
    double inter_start = max_d(target.start, sub.start);
    double inter_end = min_d(target.end, sub.end);
    
    if (inter_start >= inter_end - MIN_DURATION) { *out1 = target; return 1; }
    
    int fragments = 0;
    if (inter_start - target.start > MIN_DURATION) {
        out1->start = target.start; out1->end = inter_start; fragments++;
    }
    if (target.end - inter_end > MIN_DURATION) {
        Interval *dest = (fragments == 0) ? out1 : out2;
        dest->start = inter_end; dest->end = target.end; fragments++;
    }
    return fragments;
}

Dataset load_data(const char *upa_path, const char *time_path) {
    Dataset ds;
    memset(&ds, 0, sizeof(Dataset));
    ds.count = 0;
    ds.capacity = 8192;
    ds.entries = malloc(ds.capacity * sizeof(TupaEntry));

    printf("Loading data from:\n  UPA:  %s\n  TIME: %s\n", upa_path, time_path);
    FILE *f_upa = fopen(upa_path, "r");
    FILE *f_time = fopen(time_path, "r");
    
    if (!f_upa || !f_time) { 
        printf("CRITICAL ERROR: Could not open input files.\n"); exit(1); 
    }

    char line_upa[MAX_LINE];
    char line_time[MAX_LINE];
    
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_users_header = atoi(line_upa);
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_perms_header = atoi(line_upa);
    fgets(line_upa, MAX_LINE, f_upa); 
    
    ds.user_map_size = 10;
    ds.user_map = calloc(ds.user_map_size, sizeof(int*));
    ds.user_map_counts = calloc(ds.user_map_size, sizeof(int));
    
    ds.pid_map_size = 10;
    ds.pid_map = calloc(ds.pid_map_size, sizeof(int*));
    ds.pid_map_counts = calloc(ds.pid_map_size, sizeof(int));
    ds.pid_map_caps = calloc(ds.pid_map_size, sizeof(int));

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

// --- POST-PROCESSING ---
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

// --- CORE ALGORITHM ---
void solve_cotrapmp(Dataset *ds, double theta, Role **roles_out, int *num_roles_out) {
    int num_roles = 0;
    int roles_capacity = 1000;
    Role *roles = malloc(roles_capacity * sizeof(Role));
    
    int *checked_users = calloc(ds->max_uid_found + 2, sizeof(int)); 
    long initial_entries_to_cover = ds->count;
    long covered_count = 0;
    
    int pending_cap = 4096;
    int pending_count = 0;
    PendingEntry *pending_adds = malloc(pending_cap * sizeof(PendingEntry));

    int idx_map_cap = ds->capacity + 10000;
    int *idx_map_gen = calloc(idx_map_cap, sizeof(int));
    int current_gen = 0;

    int *cand_pids = NULL; int cp_cap = 0;
    int *cand_uids = NULL; int cu_cap = 0;

    printf("\nStarting Role Mining (Precision Fix + Dust Filter + Heuristic)...\n");
    print_progress(0, initial_entries_to_cover);

    int found_any = 1;
    while (found_any) {
        found_any = 0;
        double best_mass = -1.0; 
        Role best_role = {0};
        int found_pass = 0;
        current_gen++; 

        if (ds->count >= idx_map_cap) {
            int old_cap = idx_map_cap;
            idx_map_cap = ds->count + 20000; 
            int *new_map = realloc(idx_map_gen, idx_map_cap * sizeof(int));
            if (!new_map) { perror("OOM Map"); exit(1); }
            idx_map_gen = new_map;
            memset(idx_map_gen + old_cap, 0, (idx_map_cap - old_cap) * sizeof(int)); 
        }

        memset(checked_users, 0, (ds->max_uid_found + 1) * sizeof(int));

        int loop_bound = ds->count;
        for (int i = 0; i < loop_bound; i++) {
            if (ds->entries[i].covered) continue;
            int uid = ds->entries[i].uid;
            if (checked_users[uid]) continue; 
            checked_users[uid] = 1;
            
            Interval seed_t = ds->entries[i].t;
            int num_cp = 0;
            int *u_idx = ds->user_map[uid];
            int u_cnt = ds->user_map_counts[uid];
            
            // Strategy 1: Deep
            for(int k=0; k<u_cnt; k++) {
                int idx = u_idx[k];
                if (idx >= ds->count || ds->entries[idx].covered) continue;
                if (is_subset(ds->entries[idx].t, seed_t)) {
                    int exists=0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == ds->entries[idx].pid) { exists=1; break; }
                    if(!exists) add_to_array(&cand_pids, &num_cp, &cp_cap, ds->entries[idx].pid);
                }
            }

            if (num_cp > 0) {
                Interval role_t = { -1.0, -1.0 };
                int first_p = 1;
                for (int p = 0; p < num_cp; p++) {
                    int pid = cand_pids[p];
                    for(int k=0; k<u_cnt; k++) {
                        int idx = u_idx[k];
                        if(ds->entries[idx].pid == pid && !ds->entries[idx].covered) {
                             if(first_p) { role_t = ds->entries[idx].t; first_p=0; }
                             else {
                                 role_t.start = max_d(role_t.start, ds->entries[idx].t.start);
                                 role_t.end = min_d(role_t.end, ds->entries[idx].t.end);
                             }
                             break; 
                        }
                    }
                }

                if (duration(role_t) > MIN_DURATION) {
                    int smallest_pid = cand_pids[0];
                    int min_count = INT_MAX;
                    for(int p=0; p<num_cp; p++) {
                        int c = ds->pid_map_counts[cand_pids[p]];
                        if(c < min_count) { min_count = c; smallest_pid = cand_pids[p]; }
                    }

                    int num_cu = 0;
                    int *potential_indices = ds->pid_map[smallest_pid];
                    int potential_ct = ds->pid_map_counts[smallest_pid];

                    int fully_covered_entries = 0; 

                    for(int pi=0; pi<potential_ct; pi++) {
                        int p_idx_in_ds = potential_indices[pi];
                        if (ds->entries[p_idx_in_ds].covered) continue;
                        int u = ds->entries[p_idx_in_ds].uid;
                        
                        int already_added = 0;
                        for(int c=0; c<num_cu; c++) if(cand_uids[c] == u) { already_added=1; break; }
                        if(already_added) continue;

                        int has_all = 1;
                        int u_full_hits = 0;
                        int *chk_idx = ds->user_map[u];
                        int chk_cnt = ds->user_map_counts[u];
                        
                        for (int p_idx = 0; p_idx < num_cp; p_idx++) {
                            int needed_p = cand_pids[p_idx];
                            int p_ok = 0;
                            for(int c=0; c<chk_cnt; c++) {
                                int idx = chk_idx[c];
                                if (idx < ds->count && ds->entries[idx].pid == needed_p && !ds->entries[idx].covered) {
                                     // 2. TWEAK: Stricter Jaccard check (+1e-9) to ensure we only keep solid matches
                                     if (calc_jaccard(ds->entries[idx].t, role_t) >= theta + 1e-9) { 
                                         p_ok = 1; 
                                         if (is_subset(role_t, ds->entries[idx].t)) u_full_hits++;
                                         break; 
                                     }
                                }
                            }
                            if (!p_ok) { has_all = 0; break; }
                        }
                        if (has_all) {
                            add_to_array(&cand_uids, &num_cu, &cu_cap, u);
                            fully_covered_entries += u_full_hits;
                        }
                    }

                    // 3. TWEAK: Heuristic Bonus for full coverage
                    double raw_mass = (double)num_cu * (double)num_cp * duration(role_t);
                    double score = raw_mass * (1.0 + (0.1 * fully_covered_entries));

                    if (score > best_mass + EPSILON) {
                        if (found_pass) { if(best_role.uids) free(best_role.uids); if(best_role.pids) free(best_role.pids); }
                        best_role.uids = malloc(num_cu * sizeof(int)); memcpy(best_role.uids, cand_uids, num_cu * sizeof(int));
                        best_role.num_uids = num_cu;
                        best_role.pids = malloc(num_cp * sizeof(int)); memcpy(best_role.pids, cand_pids, num_cp * sizeof(int));
                        best_role.num_pids = num_cp;
                        best_role.t = role_t; best_role.valid = 1; best_role.uids_capacity = num_cu; best_role.pids_capacity = num_cp;
                        best_mass = score; found_pass = 1; found_any = 1;
                    }
                }
            }
            
            // Strategy 2: Broad
            int single_pid = ds->entries[i].pid;
            Interval single_t = ds->entries[i].t;
            if (duration(single_t) > MIN_DURATION) {
                int num_cu_b = 0;
                int *pot_ind_b = ds->pid_map[single_pid];
                int pot_cnt_b = ds->pid_map_counts[single_pid];
                int fully_covered_b = 0;
                
                int temp_cu_count = 0;
                
                for(int pi=0; pi<pot_cnt_b; pi++) {
                    int idx = pot_ind_b[pi];
                    if (ds->entries[idx].covered) continue;
                    int u = ds->entries[idx].uid;
                    
                    int already = 0;
                    for(int k=0; k<temp_cu_count; k++) if(cand_uids[k]==u) { already=1; break; }
                    if(already) continue;
                    
                    // Strict Jaccard Check here too
                    if (calc_jaccard(ds->entries[idx].t, single_t) >= theta + 1e-9) {
                        add_to_array(&cand_uids, &temp_cu_count, &cu_cap, u);
                         if (is_subset(single_t, ds->entries[idx].t)) fully_covered_b++;
                    }
                }
                
                double raw_mass_b = (double)temp_cu_count * 1.0 * duration(single_t);
                double score_b = raw_mass_b * (1.0 + (0.1 * fully_covered_b));

                if (score_b > best_mass + EPSILON) {
                    if (found_pass) { if(best_role.uids) free(best_role.uids); if(best_role.pids) free(best_role.pids); }
                    best_role.uids = malloc(temp_cu_count * sizeof(int)); memcpy(best_role.uids, cand_uids, temp_cu_count * sizeof(int));
                    best_role.num_uids = temp_cu_count;
                    best_role.pids = malloc(sizeof(int)); best_role.pids[0] = single_pid;
                    best_role.num_pids = 1;
                    best_role.t = single_t; best_role.valid = 1; best_role.uids_capacity = temp_cu_count; best_role.pids_capacity = 1;
                    best_mass = score_b; found_pass = 1; found_any = 1;
                }
            }
        }

        if (found_pass) {
            if (num_roles >= roles_capacity) {
                roles_capacity *= 2;
                roles = realloc(roles, roles_capacity * sizeof(Role));
            }
            best_role.id = num_roles + 1;
            roles[num_roles++] = best_role; 

            pending_count = 0; 

            for(int u_i=0; u_i<best_role.num_uids; u_i++) {
                int u = best_role.uids[u_i];
                int *chk_idx = ds->user_map[u]; 
                int chk_cnt = ds->user_map_counts[u];
                
                for(int c=0; c<chk_cnt; c++) {
                    int idx = chk_idx[c];
                    if(idx >= ds->count || ds->entries[idx].covered || idx_map_gen[idx] == current_gen) continue;
                    
                    int is_p = 0;
                    for(int p=0; p<best_role.num_pids; p++) 
                        if(best_role.pids[p] == ds->entries[idx].pid) { is_p=1; break; }
                    
                    if(is_p) {
                        TupaEntry *e = &ds->entries[idx];
                        Interval r1, r2;
                        double inter_s = max_d(e->t.start, best_role.t.start);
                        double inter_e = min_d(e->t.end, best_role.t.end);
                        
                        if (inter_e > inter_s + EPSILON) {
                            int frags = subtract_interval(e->t, best_role.t, &r1, &r2);
                            if (frags == 0) {
                                e->covered = 1;
                                if (idx < initial_entries_to_cover) covered_count++;
                            } else if (frags == 1) {
                                e->t = r1;
                            } else if (frags == 2) {
                                e->t = r1; 
                                if (pending_count >= pending_cap) {
                                    pending_cap *= 2;
                                    pending_adds = realloc(pending_adds, pending_cap * sizeof(PendingEntry));
                                }
                                pending_adds[pending_count].uid = e->uid;
                                pending_adds[pending_count].pid = e->pid;
                                pending_adds[pending_count].t = r2;
                                pending_count++;
                            }
                            idx_map_gen[idx] = current_gen; 
                        }
                    }
                }
            }
            for(int k=0; k<pending_count; k++) {
                add_entry_to_ds(ds, pending_adds[k].uid, pending_adds[k].pid, pending_adds[k].t);
            }
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
    free(pending_adds);
    free(idx_map_gen);
    
    *roles_out = roles;
    *num_roles_out = num_roles;
}

void write_outputs(Dataset *ds, Role *roles, int num_roles) {
    int active_roles = merge_roles_aggressive(roles, num_roles);
    
    printf("Writing outputs to current directory:\n  %s\n  %s\n  %s\n", OUTPUT_UA, OUTPUT_PA, OUTPUT_REB);
    FILE *ua = fopen(OUTPUT_UA, "w");
    FILE *pa = fopen(OUTPUT_PA, "w");
    FILE *reb = fopen(OUTPUT_REB, "w");
    if(!ua || !pa || !reb) { perror("Error opening output files"); exit(1); }

    int final_num_users = (ds->max_uid_found > ds->num_users_header) ? ds->max_uid_found : ds->num_users_header;
    int final_num_perms = (ds->max_pid_found > ds->num_perms_header) ? ds->max_pid_found : ds->num_perms_header;

    fprintf(ua, "%d\n%d\n", final_num_users, active_roles);
    fprintf(pa, "%d\n%d\n", active_roles, final_num_perms);
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
    printf("|PA| + |REB| = %d\n", total_pa + active_roles);
}

int main(int argc, char *argv[]) {
    if (argc < 4) { 
        printf("Usage: %s <THETA> <UPA_FILENAME> <TIME_FILENAME>\n", argv[0]); 
        printf("Note: Files must be in the '%s' folder.\n", DATA_FOLDER);
        return 1; 
    }
    
    double theta = atof(argv[1]);
    char *upa_name = argv[2];
    char *time_name = argv[3];
    
    char upa_full_path[MAX_PATH];
    char time_full_path[MAX_PATH];
    
    snprintf(upa_full_path, MAX_PATH, "%s%s", DATA_FOLDER, upa_name);
    snprintf(time_full_path, MAX_PATH, "%s%s", DATA_FOLDER, time_name);
    
    Dataset ds = load_data(upa_full_path, time_full_path);
    
    if (ds.count == 0) { printf("Error: Dataset empty.\n"); return 1; }
    printf("Dataset loaded. Entries: %d (Max UID: %d)\n", ds.count, ds.max_uid_found);
    
    Role *roles = NULL;
    int num_roles = 0;
    
    solve_cotrapmp(&ds, theta, &roles, &num_roles);
    write_outputs(&ds, roles, num_roles);
    
    free(ds.entries);
    for(int i=0; i<=ds.max_uid_found; i++) if(ds.user_map[i]) free(ds.user_map[i]);
    free(ds.user_map); free(ds.user_map_counts);
    for(int i=0; i<ds.pid_map_size; i++) if(ds.pid_map[i]) free(ds.pid_map[i]);
    free(ds.pid_map); free(ds.pid_map_counts); free(ds.pid_map_caps);
    for(int i=0; i<num_roles; i++) { if(roles[i].uids) free(roles[i].uids); if(roles[i].pids) free(roles[i].pids); }
    free(roles);
    return 0;
}