#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h> // REQUIRED: Multi-threading

// --- Configuration ---
#define MAX_LINE 8192
#define MAX_PATH 1024
#define EPSILON 1e-6 
#define BASE_DIR "./" 

// Output config
#define OUTPUT_UA "UA_TIME.txt"
#define OUTPUT_PA "PA_TIME.txt"
#define OUTPUT_REB "REB_TIME.txt"

// Tuning
#define MIN_ROLE_AREA 0.0001

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

// OPTIMIZATION 1: Inverted Index Structure
typedef struct {
    int **pid_to_uids;      // [pid] -> array of uids
    int *pid_to_uids_count; // [pid] -> count
    int *pid_to_uids_cap;   // [pid] -> capacity
} InvertedIndex;

typedef struct {
    int *uids;
    int num_uids;
    int *pids;
    int num_pids;
    Interval t;
    
    int uids_capacity;
    int pids_capacity;

    int *temp_covered_indices;
    int temp_num_covered;
    int temp_covered_cap;
    
    double area;
    int valid; 
    int id;
} Role;

// --- Generic Helpers ---

double max_d(double a, double b) { return (a > b) ? a : b; }
double min_d(double a, double b) { return (a < b) ? a : b; }

double duration(Interval iv) {
    double d = iv.end - iv.start;
    return (d < 0) ? 0.0 : d;
}

int compare_ints(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

// Optimized add_unique
void add_unique(int **arr, int *count, int *capacity, int val) {
    for(int i=0; i<*count; i++) if ((*arr)[i] == val) return;
    
    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? 16 : (*capacity * 2);
        int *new_arr = realloc(*arr, *capacity * sizeof(int));
        if (!new_arr) exit(1);
        *arr = new_arr;
    }
    (*arr)[*count] = val;
    (*count)++;
}

// --- Set Helpers (Added for Output Compatibility) ---

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

// --- Inverted Index Functions ---

void build_inverted_index(Dataset *ds, InvertedIndex *idx) {
    idx->pid_to_uids = calloc(ds->num_perms + 1, sizeof(int*));
    idx->pid_to_uids_count = calloc(ds->num_perms + 1, sizeof(int));
    idx->pid_to_uids_cap = calloc(ds->num_perms + 1, sizeof(int));

    int *last_added_user = malloc((ds->num_perms + 1) * sizeof(int));
    for(int i=0; i<=ds->num_perms; i++) last_added_user[i] = -1;

    for(int u=1; u<=ds->num_users; u++) {
        int u_cnt = ds->user_map_counts[u];
        for(int k=0; k<u_cnt; k++) {
            int entry_idx = ds->user_map[u][k];
            int pid = ds->entries[entry_idx].pid;

            if (last_added_user[pid] != u) {
                add_unique(&idx->pid_to_uids[pid], 
                           &idx->pid_to_uids_count[pid], 
                           &idx->pid_to_uids_cap[pid], u);
                last_added_user[pid] = u;
            }
        }
    }
    free(last_added_user);
}

void free_inverted_index(InvertedIndex *idx, int num_perms) {
    for(int i=0; i<=num_perms; i++) {
        if(idx->pid_to_uids[i]) free(idx->pid_to_uids[i]);
    }
    free(idx->pid_to_uids);
    free(idx->pid_to_uids_count);
    free(idx->pid_to_uids_cap);
}

// --- Data Loading ---

int add_entry_to_ds(Dataset *ds, int uid, int pid, Interval t) {
    if (ds->count >= ds->capacity) {
        ds->capacity *= 2;
        ds->entries = realloc(ds->entries, ds->capacity * sizeof(TupaEntry));
    }
    int idx = ds->count;
    ds->entries[idx] = (TupaEntry){uid, pid, t, idx, 0};
    ds->count++;
    
    if (uid > ds->num_users) { /* dynamic resizing omitted for brevity */ }
    
    int *bucket = ds->user_map[uid];
    int cnt = ds->user_map_counts[uid];
    bucket = realloc(bucket, (cnt + 1) * sizeof(int));
    bucket[cnt] = idx;
    ds->user_map[uid] = bucket;
    ds->user_map_counts[uid]++;
    
    return idx;
}

Dataset load_data(const char *upa_path, const char *time_path) {
    Dataset ds = {0};
    ds.capacity = 8192;
    ds.entries = malloc(ds.capacity * sizeof(TupaEntry));

    char upa_full_path[MAX_PATH], time_full_path[MAX_PATH];
    sprintf(upa_full_path, "%s%s", BASE_DIR, upa_path);
    sprintf(time_full_path, "%s%s", BASE_DIR, time_path);

    printf("Loading data from %s...\n", upa_path);
    FILE *f_upa = fopen(upa_full_path, "r");
    FILE *f_time = fopen(time_full_path, "r");
    if (!f_upa || !f_time) { printf("Error opening files.\n"); exit(1); }

    char buf[MAX_LINE];
    if(fgets(buf, MAX_LINE, f_upa)) ds.num_users = atoi(buf);
    if(fgets(buf, MAX_LINE, f_upa)) ds.num_perms = atoi(buf);
    fgets(buf, MAX_LINE, f_upa); 

    ds.user_map = calloc((ds.num_users + 100), sizeof(int*));
    ds.user_map_counts = calloc((ds.num_users + 100), sizeof(int));

    char l_time[MAX_LINE];
    while (fgets(buf, MAX_LINE, f_upa) && fgets(l_time, MAX_LINE, f_time)) {
        int u, p;
        if (sscanf(buf, "%d %d", &u, &p) != 2) continue;
        
        char *ptr = l_time;
        double s, e;
        int offset;
        while (sscanf(ptr, "%lf %lf%n", &s, &e, &offset) == 2) {
            add_entry_to_ds(&ds, u, p, (Interval){s, e});
            ptr += offset;
        }
    }
    fclose(f_upa); fclose(f_time);
    return ds;
}

// --- Post-Processing (Aggressive Merge) ---

int merge_roles_aggressive(Role *roles, int num_roles) {
    int merged_count = 0;
    printf("\nPost-processing: Aggressive Merging...\n");

    // Sort UIDs and PIDs within each role for set comparison
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
                        union_sorted_sets(&roles[i], &roles[j], 1); // Merge PIDs
                        roles[j].valid = 0; merged_count++; change = 1; continue;
                    }
                    if (are_sets_equal_sorted(roles[i].pids, roles[i].num_pids, roles[j].pids, roles[j].num_pids)) {
                        union_sorted_sets(&roles[i], &roles[j], 0); // Merge UIDs
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

// --- Parallelized Miner ---

void solve_cotrapmp(Dataset *ds, double theta, Role **roles_out, int *num_roles_out) {
    printf("\nStarting Optimized FastMiner (Theta=%.2f)...\n", theta);
    
    InvertedIndex idx;
    build_inverted_index(ds, &idx);

    int roles_cap = 1024;
    Role *roles = malloc(roles_cap * sizeof(Role));
    int num_roles = 0;
    int iteration = 0;
    int max_threads = omp_get_max_threads();
    printf("Parallel execution with %d threads.\n", max_threads);

    while (1) {
        iteration++;
        
        int uncovered_count = 0;
        for(int i=0; i<ds->count; i++) if(!ds->entries[i].covered) uncovered_count++;
        
        if (uncovered_count == 0) break;
        
        printf("\rIter %d | Uncovered: %d", iteration, uncovered_count);
        fflush(stdout);

        Role global_best = {0};
        global_best.area = -1.0;

        Role *thread_bests = calloc(max_threads, sizeof(Role));

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            Role local_best = {0};
            local_best.area = -1.0;

            int *cand_pids = NULL; int cand_p_cap = 0;
            int *cand_uids = NULL; int cand_u_cap = 0;
            int *temp_indices = NULL; int temp_idx_cap = 0;

            #pragma omp for schedule(dynamic, 5)
            for (int u = 1; u <= ds->num_users; u++) {
                int u_cnt = ds->user_map_counts[u];
                if (u_cnt == 0) continue;

                int has_uncovered = 0;
                for(int k=0; k<u_cnt; k++) if (!ds->entries[ds->user_map[u][k]].covered) { has_uncovered = 1; break; }
                if (!has_uncovered) continue;

                for (int k = 0; k < u_cnt; k++) {
                    int seed_idx = ds->user_map[u][k];
                    TupaEntry *seed_e = &ds->entries[seed_idx];
                    if (seed_e->covered) continue;

                    // A. Find Permissions P
                    int cp_count = 0;
                    add_unique(&cand_pids, &cp_count, &cand_p_cap, seed_e->pid);
                    
                    for (int kk = 0; kk < u_cnt; kk++) {
                        int e2_idx = ds->user_map[u][kk];
                        if (ds->entries[e2_idx].covered) continue;
                        TupaEntry *e2 = &ds->entries[e2_idx];
                        double i_s = max_d(seed_e->t.start, e2->t.start);
                        double i_e = min_d(seed_e->t.end, e2->t.end);
                        if (i_e <= i_s) continue;
                        double u_s = min_d(seed_e->t.start, e2->t.start);
                        double u_e = max_d(seed_e->t.end, e2->t.end);
                        if ((i_e - i_s) / (u_e - u_s) >= theta) add_unique(&cand_pids, &cp_count, &cand_p_cap, e2->pid);
                    }

                    // B. Determine T (Simplified to Seed T)
                    double t_start = seed_e->t.start;
                    double t_end = seed_e->t.end;
                    
                    // C. Find Users U (Using Index)
                    int *potential_users = idx.pid_to_uids[seed_e->pid];
                    int pot_count = idx.pid_to_uids_count[seed_e->pid];
                    int cu_count = 0;

                    for (int i_pot = 0; i_pot < pot_count; i_pot++) {
                        int target_u = potential_users[i_pot];
                        int target_u_cnt = ds->user_map_counts[target_u];
                        int has_all = 1;
                        for (int p_i = 0; p_i < cp_count; p_i++) {
                            int needed_p = cand_pids[p_i];
                            int found_p = 0;
                            for(int z=0; z<target_u_cnt; z++) {
                                TupaEntry *te = &ds->entries[ds->user_map[target_u][z]];
                                if (te->pid == needed_p && !te->covered) {
                                    double i_s = max_d(te->t.start, t_start);
                                    double i_e = min_d(te->t.end, t_end);
                                    if (i_e > i_s) {
                                         double u_s = min_d(te->t.start, t_start);
                                         double u_e = max_d(te->t.end, t_end);
                                         if ((i_e-i_s)/(u_e-u_s) >= theta) { found_p = 1; break; }
                                    }
                                }
                            }
                            if (!found_p) { has_all = 0; break; }
                        }
                        if (has_all) add_unique(&cand_uids, &cu_count, &cand_u_cap, target_u);
                    }

                    double area = (double)cu_count * (double)cp_count * (t_end - t_start);
                    if (area > local_best.area) {
                        if(local_best.uids) free(local_best.uids);
                        if(local_best.pids) free(local_best.pids);
                        local_best.area = area;
                        local_best.t.start = t_start; local_best.t.end = t_end;
                        local_best.num_uids = cu_count;
                        local_best.uids = malloc(cu_count * sizeof(int));
                        memcpy(local_best.uids, cand_uids, cu_count * sizeof(int));
                        local_best.num_pids = cp_count;
                        local_best.pids = malloc(cp_count * sizeof(int));
                        memcpy(local_best.pids, cand_pids, cp_count * sizeof(int));
                        local_best.valid = 1;
                    }
                }
            } 
            thread_bests[tid] = local_best;
            if (cand_pids) free(cand_pids);
            if (cand_uids) free(cand_uids);
            if (temp_indices) free(temp_indices);
        } 

        for(int t=0; t<max_threads; t++) {
            if (thread_bests[t].valid && thread_bests[t].area > global_best.area) {
                if (global_best.uids) free(global_best.uids);
                if (global_best.pids) free(global_best.pids);
                global_best = thread_bests[t]; 
                thread_bests[t].uids = NULL; thread_bests[t].pids = NULL;
            } else {
                if (thread_bests[t].uids) free(thread_bests[t].uids);
                if (thread_bests[t].pids) free(thread_bests[t].pids);
            }
        }
        free(thread_bests);

        if (!global_best.valid || global_best.area <= MIN_ROLE_AREA) break;

        if (num_roles >= roles_cap) {
            roles_cap *= 2;
            roles = realloc(roles, roles_cap * sizeof(Role));
        }
        roles[num_roles] = global_best;
        roles[num_roles].id = num_roles + 1;
        num_roles++;

        for (int i=0; i<global_best.num_uids; i++) {
            int u = global_best.uids[i];
            int u_cnt = ds->user_map_counts[u];
            for(int p_i=0; p_i<global_best.num_pids; p_i++) {
                int pid = global_best.pids[p_i];
                for(int k=0; k<u_cnt; k++) {
                    int e_idx = ds->user_map[u][k];
                    TupaEntry *entry = &ds->entries[e_idx];
                    if (entry->pid != pid || entry->covered) continue;
                    double is = max_d(entry->t.start, global_best.t.start);
                    double ie = min_d(entry->t.end, global_best.t.end);
                    if (ie > is + EPSILON) {
                        if (is <= entry->t.start + EPSILON && ie >= entry->t.end - EPSILON) {
                            entry->covered = 1;
                        } else {
                             if (is > entry->t.start + EPSILON) add_entry_to_ds(ds, u, pid, (Interval){entry->t.start, is});
                             if (ie < entry->t.end - EPSILON) add_entry_to_ds(ds, u, pid, (Interval){ie, entry->t.end});
                             entry->covered = 1;
                        }
                    }
                }
            }
        }
    }

    printf("\nDone. Found %d roles.\n", num_roles);
    free_inverted_index(&idx, ds->num_perms);
    *roles_out = roles;
    *num_roles_out = num_roles;
}

// --- Output Function (Updated to match Model Output) ---

void write_outputs(Dataset *ds, Role *roles, int num_roles) {
    // 1. Call Aggressive Merge first
    int active_roles = merge_roles_aggressive(roles, num_roles);
    
    FILE *ua = fopen(OUTPUT_UA, "w");
    FILE *pa = fopen(OUTPUT_PA, "w");
    FILE *reb = fopen(OUTPUT_REB, "w");
    
    fprintf(ua, "%d\n%d\n", ds->num_users, active_roles);
    fprintf(pa, "%d\n%d\n", active_roles, ds->num_perms);
    fprintf(reb, "%d\n", active_roles);

    int rid = 1;
    int total_pa_count = 0; // Track for stats

    for(int i=0; i<num_roles; i++) {
        if (!roles[i].valid) continue;
        
        for(int j=0; j<roles[i].num_uids; j++) fprintf(ua, "u%d r%d\n", roles[i].uids[j], rid);
        
        for(int j=0; j<roles[i].num_pids; j++) {
            fprintf(pa, "p%d r%d\n", roles[i].pids[j], rid);
            total_pa_count++;
        }
        
        fprintf(reb, "r%d %.2f %.2f\n", rid, roles[i].t.start, roles[i].t.end);
        rid++;
    }
    fclose(ua); fclose(pa); fclose(reb);

    // 2. Print the exact stats expected by Python script
    printf("--- Final Stats ---\n");
    printf("|R| = %d\n", active_roles);
    printf("|PA| + |REB| = %d\n", total_pa_count + active_roles);
}

int main(int argc, char *argv[]) {
    if(argc < 2) { printf("Usage: %s <theta>\n", argv[0]); return 1; }
    double theta = atof(argv[1]);
    Dataset ds = load_data("apj.txt", "TIME_10int_c25_o75_apj.txt");
    Role *roles; int num_roles;
    
    solve_cotrapmp(&ds, theta, &roles, &num_roles);
    
    write_outputs(&ds, roles, num_roles);
    return 0;
}