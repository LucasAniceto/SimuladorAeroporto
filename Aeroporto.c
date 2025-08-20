#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#define TIMEOUT_QUEDA 90        
#define TEMPO_ALERTA 60        
#define TIMEOUT_BACKOFF 6   

#define NUM_PISTAS 3           
#define NUM_PORTOES 5          
#define CAPACIDADE_TORRE 2      

#define INTERVALO_MIN_MS 500    
#define INTERVALO_MAX_MS 1500   

#define TEMPO_POUSO_MIN 3       
#define TEMPO_POUSO_VAR 6       
#define TEMPO_DESEMB_MIN 3      
#define TEMPO_DESEMB_VAR 5      
#define TEMPO_DECOL_MIN 2       
#define TEMPO_DECOL_VAR 4       

#define MAX_AVIOES 1000
#define VOO_DOMESTICO 0
#define VOO_INTERNACIONAL 1

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_cond_t cond_dom;  
    int available;
    int waiting_dom, waiting_int;
    time_t oldest_dom_time;
} resource_t;

typedef struct {
    int id, type;
    pthread_t thread_id;
    time_t tempo_inicio;
    int estado; 
} airplane_t;

resource_t pistas, portoes, torre;
airplane_t avioes[MAX_AVIOES];
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
int total_avioes = 0, sucessos = 0, quedas = 0, ativos = 0;
int domesticos = 0, internacionais = 0;
int alertas_criticos = 0, deadlocks_detectados = 0, starvation_casos = 0;
int preempcoes_realizadas = 0; 
int deadlocks_evitados = 0; 
int deadlocks_resolvidos = 0; 
int num_pistas = NUM_PISTAS, num_portoes = NUM_PORTOES, capacidade_torre = CAPACIDADE_TORRE, tempo_sim = 300;
int intervalo_min = INTERVALO_MIN_MS;
int intervalo_max = INTERVALO_MAX_MS;
int simulation_running = 1, airplane_counter = 0;
time_t start_time;

typedef struct critical_airplane {
    int aviao_id;
    time_t tempo_critico;
    struct critical_airplane* next;
} critical_airplane_t;

critical_airplane_t* critical_list = NULL;
pthread_mutex_t critical_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t avioes_mutex = PTHREAD_MUTEX_INITIALIZER; 

typedef struct resource_holder {
    int aviao_id;
    int recurso_tipo; 
    struct resource_holder* next;
} resource_holder_t;

typedef struct waiting_thread {
    int aviao_id;
    int recurso_tipo;
    time_t tempo_espera; 
    struct waiting_thread* next;
} waiting_thread_t;

resource_holder_t* pistas_holders = NULL;
resource_holder_t* portoes_holders = NULL; 
resource_holder_t* torre_holders = NULL;
waiting_thread_t* waiting_threads = NULL;

pthread_mutex_t deadlock_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_resource(resource_t* res, int capacity, int is_torre);
int acquire_res(resource_t* res, int type, int timeout, int is_torre, int aviao_id, time_t tempo_inicio);
int acquire_with_backoff(resource_t* res1, resource_t* res2, int type, int is_torre1, int is_torre2, int aviao_id, time_t tempo_inicio);
int acquire_three_resources(resource_t* res1, resource_t* res2, resource_t* res3, int type, int is_torre1, int is_torre2, int is_torre3, int aviao_id, time_t tempo_inicio);
void release_res(resource_t* res, int type, int is_torre, int aviao_id);
void* airplane_thread(void* arg);
void* monitor_thread(void* arg);
void log_msg(const char* msg);
void update_stats(int status, int type);

void add_to_critical_list(int aviao_id, time_t tempo_critico);
void remove_from_critical_list(int aviao_id);
int check_preemption_needed();
int force_preemption(int critical_aviao_id);
int force_preemption_by_id(int victim_id);  
void* aging_thread(void* arg);

void add_resource_holder(int aviao_id, int recurso_tipo);
void remove_resource_holder(int aviao_id, int recurso_tipo);
void add_waiting_thread(int aviao_id, int recurso_tipo);
void remove_waiting_thread(int aviao_id);
int detect_deadlock();
int resolve_deadlock(int aviao1_id, int aviao2_id); 
void* deadlock_detection_thread(void* arg);

void log_msg(const char* msg) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    printf("[%02d:%02d:%02d] %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
    fflush(stdout);
}

void init_resource(resource_t* res, int capacity, int is_torre) {
    pthread_mutex_init(&res->mutex, NULL);
    pthread_cond_init(&res->cond, NULL);
    if (is_torre) pthread_cond_init(&res->cond_dom, NULL);
    res->available = capacity;
    res->waiting_dom = res->waiting_int = 0;
    res->oldest_dom_time = 0;
}

int acquire_res(resource_t* res, int type, int timeout, int is_torre, int aviao_id, time_t tempo_inicio) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout;
    int alerta_enviado = 0;
    time_t tempo_entrada_loop = time(NULL);
    
    pthread_mutex_lock(&res->mutex);
    
    if (type == VOO_DOMESTICO) {
        res->waiting_dom++;
        if (res->oldest_dom_time == 0) {
            res->oldest_dom_time = time(NULL);
        }
    } else {
        res->waiting_int++;
    }
    
    int recurso_tipo = -1;
    if (res == &pistas) recurso_tipo = 0;
    else if (res == &portoes) recurso_tipo = 1;
    else if (res == &torre) recurso_tipo = 2;
    
    int foi_adicionado_espera = 0;
    
    while (res->available <= 0 && simulation_running) {
        if (!foi_adicionado_espera && recurso_tipo != -1) {
            add_waiting_thread(aviao_id, recurso_tipo);
            foi_adicionado_espera = 1;
        }
        
        time_t agora = time(NULL);
        time_t tempo_vida = agora - tempo_inicio;
        time_t tempo_esperando = agora - tempo_entrada_loop;
        
        if (tempo_vida >= TIMEOUT_QUEDA) {
            char msg[150];
            snprintf(msg, sizeof(msg), "STARVATION: Aviao %d (%s) caiu - Tempo vida: %lds, Esperando: %lds", 
                     aviao_id, type ? "INTL" : "DOM", tempo_vida, tempo_esperando);
            log_msg(msg);
            pthread_mutex_lock(&stats_mutex);
            starvation_casos++;
            pthread_mutex_unlock(&stats_mutex);
            
            if (type == VOO_DOMESTICO) {
                res->waiting_dom--;
                if (res->waiting_dom == 0) res->oldest_dom_time = 0;
                remove_from_critical_list(aviao_id);
            } else {
                res->waiting_int--;
            }
            
            if (foi_adicionado_espera) {
                remove_waiting_thread(aviao_id);
            }
            
            pthread_mutex_unlock(&res->mutex);
            return -1;
        }
        
        if (tempo_vida >= TEMPO_ALERTA && !alerta_enviado) {
            char msg[150];
            snprintf(msg, sizeof(msg), "ALERTA CRITICO: Aviao %d (%s) vida: %lds, esperando: %lds", 
                     aviao_id, type ? "INTL" : "DOM", tempo_vida, tempo_esperando);
            log_msg(msg);
            pthread_mutex_lock(&stats_mutex);
            alertas_criticos++;
            pthread_mutex_unlock(&stats_mutex);
            alerta_enviado = 1;
            
            if (type == VOO_DOMESTICO) {
                add_to_critical_list(aviao_id, agora);
            }
        }
        
        struct timespec ts_curto;
        clock_gettime(CLOCK_REALTIME, &ts_curto);
        ts_curto.tv_sec += 1; 
        
        if (type == VOO_DOMESTICO && is_torre) {
            if (pthread_cond_timedwait(&res->cond_dom, &res->mutex, &ts_curto) == ETIMEDOUT) {
                continue;
            }
        } else {
            if (pthread_cond_timedwait(&res->cond, &res->mutex, &ts_curto) == ETIMEDOUT) {
                continue;
            }
        }
    }
    
    if (!simulation_running || res->available <= 0) {
        if (type == VOO_DOMESTICO) {
            res->waiting_dom--;
            if (res->waiting_dom == 0) res->oldest_dom_time = 0;
            remove_from_critical_list(aviao_id);
        } else {
            res->waiting_int--;
        }
        
        if (foi_adicionado_espera) {
            remove_waiting_thread(aviao_id);
        }
        
        pthread_mutex_unlock(&res->mutex);
        return -1;
    }
    
    if (type == VOO_DOMESTICO) {
        res->waiting_dom--;
        if (res->waiting_dom == 0) res->oldest_dom_time = 0;
        remove_from_critical_list(aviao_id);
    } else {
        res->waiting_int--;
    }
    
    if (foi_adicionado_espera) {
        remove_waiting_thread(aviao_id);
    }
    if (recurso_tipo != -1) {
        add_resource_holder(aviao_id, recurso_tipo);
    }
    
    res->available--;
    pthread_mutex_unlock(&res->mutex);
    return 0;
}

void release_res(resource_t* res, int type, int is_torre, int aviao_id) {
    pthread_mutex_lock(&res->mutex);
    
    int recurso_tipo = -1;
    if (res == &pistas) recurso_tipo = 0;
    else if (res == &portoes) recurso_tipo = 1;
    else if (res == &torre) recurso_tipo = 2;
    
    if (recurso_tipo != -1) {
        remove_resource_holder(aviao_id, recurso_tipo);
    }
    
    res->available++;
    
    if (is_torre) {
        if (res->waiting_int > 0) {
            pthread_cond_signal(&res->cond); 
        } else if (res->waiting_dom > 0) {
            pthread_cond_signal(&res->cond_dom); 
        } else {
            pthread_cond_broadcast(&res->cond);
            pthread_cond_broadcast(&res->cond_dom);
        }
    } else {
        if (res->waiting_int > 0) {
            pthread_cond_signal(&res->cond); 
        } else {
            pthread_cond_signal(&res->cond); 
        }
    }
    
    pthread_mutex_unlock(&res->mutex);
}

int acquire_with_backoff(resource_t* res1, resource_t* res2, int type, int is_torre1, int is_torre2, int aviao_id, time_t tempo_inicio) {
    int max_tentativas = 20; 
    int tentativa = 0;
    
    while (tentativa < max_tentativas && simulation_running) {
        time_t tempo_vida = time(NULL) - tempo_inicio;
        if (tempo_vida >= TIMEOUT_QUEDA) {
            return -1; 
        }
        
        if (acquire_res(res1, type, TIMEOUT_BACKOFF, is_torre1, aviao_id, tempo_inicio) != 0) {
            usleep(500000 + rand() % 500000); 
            tentativa++;
            continue;
        }
        
        if (acquire_res(res2, type, TIMEOUT_BACKOFF, is_torre2, aviao_id, tempo_inicio) == 0) {
            return 0;
        }
        
        release_res(res1, type, is_torre1, aviao_id);
        
        char msg[150];
        snprintf(msg, sizeof(msg), "BACKOFF: Aviao %d (%s) liberou recursos para evitar deadlock (tentativa %d)", 
                 aviao_id, type ? "INTL" : "DOM", tentativa + 1);
        log_msg(msg);
        
        usleep(200000 + rand() % 300000); 
        tentativa++;
        
        pthread_mutex_lock(&stats_mutex);
        deadlocks_evitados++; 
        pthread_mutex_unlock(&stats_mutex);
    }
    
    return -1;
}

int acquire_three_resources(resource_t* res1, resource_t* res2, resource_t* res3, int type, int is_torre1, int is_torre2, int is_torre3, int aviao_id, time_t tempo_inicio) {
    int max_tentativas = 20;
    int tentativa = 0;
    
    while (tentativa < max_tentativas && simulation_running) {
        time_t tempo_vida = time(NULL) - tempo_inicio;
        if (tempo_vida >= TIMEOUT_QUEDA) {
            return -1;
        }
        
        if (acquire_res(res1, type, TIMEOUT_BACKOFF, is_torre1, aviao_id, tempo_inicio) != 0) {
            usleep(500000 + rand() % 500000); 
            tentativa++;
            continue;
        }
        
        if (acquire_res(res2, type, TIMEOUT_BACKOFF, is_torre2, aviao_id, tempo_inicio) != 0) {
            release_res(res1, type, is_torre1, aviao_id);
            char msg[150];
            snprintf(msg, sizeof(msg), "BACKOFF: Aviao %d (%s) liberou recurso 1 (decolagem tentativa %d)", 
                     aviao_id, type ? "INTL" : "DOM", tentativa + 1);
            log_msg(msg);
            usleep(200000 + rand() % 300000); 
            tentativa++;
            continue;
        }
        
        if (acquire_res(res3, type, TIMEOUT_BACKOFF, is_torre3, aviao_id, tempo_inicio) == 0) {
            return 0;
        }
        
        release_res(res2, type, is_torre2, aviao_id);
        release_res(res1, type, is_torre1, aviao_id);
        
        char msg[150];
        snprintf(msg, sizeof(msg), "BACKOFF: Aviao %d (%s) liberou recursos 1+2 (decolagem tentativa %d)", 
                 aviao_id, type ? "INTL" : "DOM", tentativa + 1);
        log_msg(msg);
        
        usleep(200000 + rand() % 300000); 
        tentativa++;
        
        pthread_mutex_lock(&stats_mutex);
        deadlocks_evitados++; 
        pthread_mutex_unlock(&stats_mutex);
    }
    
    return -1;
}





void* airplane_thread(void* arg) {
    airplane_t* plane = (airplane_t*)arg;
    char msg[100];
    
    pthread_mutex_lock(&avioes_mutex);
    plane->tempo_inicio = time(NULL);
    plane->estado = 0;
    pthread_mutex_unlock(&avioes_mutex);
    
    pthread_mutex_lock(&stats_mutex);
    ativos++;
    pthread_mutex_unlock(&stats_mutex);
    
    snprintf(msg, sizeof(msg), "Aviao %d (%s): Iniciando", 
             plane->id, plane->type ? "INTL" : "DOM");
    log_msg(msg);
    
    pthread_mutex_lock(&avioes_mutex);
    plane->estado = 0;
    pthread_mutex_unlock(&avioes_mutex);
    int pouso_result;
    if (plane->type == VOO_INTERNACIONAL) {
        pouso_result = acquire_with_backoff(&pistas, &torre, plane->type, 0, 1, plane->id, plane->tempo_inicio);
    } else {
        pouso_result = acquire_with_backoff(&torre, &pistas, plane->type, 1, 0, plane->id, plane->tempo_inicio);
    }
    
    if (pouso_result == 0) {
        char msg[100];
        snprintf(msg, sizeof(msg), "Aviao %d: POUSANDO", plane->id);
        log_msg(msg);
        sleep(TEMPO_POUSO_MIN + rand() % TEMPO_POUSO_VAR);
        
        release_res(&pistas, plane->type, 0, plane->id);
        release_res(&torre, plane->type, 1, plane->id);
    }
    
    if (pouso_result != 0) {
        pthread_mutex_lock(&avioes_mutex);
        plane->estado = -1;
        pthread_mutex_unlock(&avioes_mutex);
        time_t tempo_total = time(NULL) - plane->tempo_inicio;
        snprintf(msg, sizeof(msg), "Aviao %d: QUEDA (tempo total: %lds)", plane->id, tempo_total);
        log_msg(msg);
        update_stats(-1, plane->type);
        pthread_mutex_lock(&stats_mutex);
        ativos--;
        pthread_mutex_unlock(&stats_mutex);
        return NULL;
    }
    
    pthread_mutex_lock(&avioes_mutex);
    plane->estado = 1;
    pthread_mutex_unlock(&avioes_mutex);
    int desembarque_result;
    if (plane->type == VOO_INTERNACIONAL) {
        desembarque_result = acquire_with_backoff(&portoes, &torre, plane->type, 0, 1, plane->id, plane->tempo_inicio);
    } else {
        desembarque_result = acquire_with_backoff(&torre, &portoes, plane->type, 1, 0, plane->id, plane->tempo_inicio);
    }
    
    if (desembarque_result == 0) {
        char msg[100];
        snprintf(msg, sizeof(msg), "Aviao %d: DESEMBARCANDO", plane->id);
        log_msg(msg);
        sleep(TEMPO_DESEMB_MIN + rand() % TEMPO_DESEMB_VAR);
        release_res(&torre, plane->type, 1, plane->id);
        sleep(1);
        release_res(&portoes, plane->type, 0, plane->id);
    }
    
    if (desembarque_result != 0) {
        pthread_mutex_lock(&avioes_mutex);
        plane->estado = -1;
        pthread_mutex_unlock(&avioes_mutex);
        time_t tempo_total = time(NULL) - plane->tempo_inicio;
        snprintf(msg, sizeof(msg), "Aviao %d: QUEDA (tempo total: %lds)", plane->id, tempo_total);
        log_msg(msg);
        update_stats(-1, plane->type);
        pthread_mutex_lock(&stats_mutex);
        ativos--;
        pthread_mutex_unlock(&stats_mutex);
        return NULL;
    }
    
    pthread_mutex_lock(&avioes_mutex);
    plane->estado = 2;
    pthread_mutex_unlock(&avioes_mutex);
    int decolagem_result;
    if (plane->type == VOO_INTERNACIONAL) {
        decolagem_result = acquire_three_resources(&portoes, &pistas, &torre, plane->type, 0, 0, 1, plane->id, plane->tempo_inicio);
    } else {
        decolagem_result = acquire_three_resources(&torre, &portoes, &pistas, plane->type, 1, 0, 0, plane->id, plane->tempo_inicio);
    }
    
    if (decolagem_result == 0) {
        char msg[100];
        snprintf(msg, sizeof(msg), "Aviao %d: DECOLANDO", plane->id);
        log_msg(msg);
        sleep(TEMPO_DECOL_MIN + rand() % TEMPO_DECOL_VAR);
        
        release_res(&pistas, plane->type, 0, plane->id);
        release_res(&torre, plane->type, 1, plane->id);
        release_res(&portoes, plane->type, 0, plane->id);
    }
    
    if (decolagem_result != 0) {
        pthread_mutex_lock(&avioes_mutex);
        plane->estado = -1;
        pthread_mutex_unlock(&avioes_mutex);
        time_t tempo_total = time(NULL) - plane->tempo_inicio;
        snprintf(msg, sizeof(msg), "Aviao %d: QUEDA (tempo total: %lds)", plane->id, tempo_total);
        log_msg(msg);
        update_stats(-1, plane->type);
        pthread_mutex_lock(&stats_mutex);
        ativos--;
        pthread_mutex_unlock(&stats_mutex);
        return NULL;
    }
    
    pthread_mutex_lock(&avioes_mutex);
    plane->estado = 3;
    pthread_mutex_unlock(&avioes_mutex);
    time_t tempo_total = time(NULL) - plane->tempo_inicio;
    snprintf(msg, sizeof(msg), "Aviao %d: SUCESSO (tempo total: %lds)", plane->id, tempo_total);
    log_msg(msg);
    update_stats(1, plane->type);
    
    pthread_mutex_lock(&stats_mutex);
    ativos--;
    pthread_mutex_unlock(&stats_mutex);
    return NULL;
}

void* monitor_thread(void* arg __attribute__((unused))) {
    while (simulation_running) {
        sleep(15);
        
        pthread_mutex_lock(&stats_mutex);
        printf("\n*** STATUS ***\n");
        printf("Total: %d | Ativos: %d | Sucessos: %d | Quedas: %d\n", 
               total_avioes, ativos, sucessos, quedas);
        printf("Domesticos: %d | Internacionais: %d\n", domesticos, internacionais);
        printf("Alertas: %d | Starvation: %d | DL Det: %d | DL Res: %d | DL Evit: %d | Preempções: %d\n", 
               alertas_criticos, starvation_casos, deadlocks_detectados, deadlocks_resolvidos, deadlocks_evitados, preempcoes_realizadas);
        
        int elapsed = time(NULL) - start_time;
        int remaining = tempo_sim - elapsed;
        printf("Tempo restante: %02d:%02d\n", remaining / 60, remaining % 60);
        printf("==================================\n");
        fflush(stdout);
        pthread_mutex_unlock(&stats_mutex);
    }
    return NULL;
}

void update_stats(int status, int type) {
    pthread_mutex_lock(&stats_mutex);
    total_avioes++;
    if (type == VOO_DOMESTICO) domesticos++;
    else internacionais++;
    if (status == 1) sucessos++;
    else if (status == -1) quedas++;
    pthread_mutex_unlock(&stats_mutex);
}

void add_to_critical_list(int aviao_id, time_t tempo_critico) {
    pthread_mutex_lock(&critical_mutex);
    
    critical_airplane_t* new_critical = malloc(sizeof(critical_airplane_t));
    new_critical->aviao_id = aviao_id;
    new_critical->tempo_critico = tempo_critico;
    new_critical->next = critical_list;
    critical_list = new_critical;
    
    char msg[150];
    snprintf(msg, sizeof(msg), "AGING: Aviao %d adicionado à lista crítica", aviao_id);
    log_msg(msg);
    
    pthread_mutex_unlock(&critical_mutex);
}

void remove_from_critical_list(int aviao_id) {
    pthread_mutex_lock(&critical_mutex);
    
    critical_airplane_t* current = critical_list;
    critical_airplane_t* prev = NULL;
    
    while (current != NULL) {
        if (current->aviao_id == aviao_id) {
            if (prev == NULL) {
                critical_list = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&critical_mutex);
}

int check_preemption_needed() {
    pthread_mutex_lock(&critical_mutex);
    
    critical_airplane_t* current = critical_list;
    time_t now = time(NULL);
    
    while (current != NULL) {
        if (now - current->tempo_critico >= 2) { 
            int victim_id = current->aviao_id;
            pthread_mutex_unlock(&critical_mutex);
            return victim_id;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&critical_mutex);
    return -1;
}

int force_preemption(int critical_aviao_id) {
    pthread_mutex_lock(&avioes_mutex);
    
    for (int i = 0; i < airplane_counter; i++) {
        if (avioes[i].type == VOO_INTERNACIONAL && 
            (avioes[i].estado == 0 || avioes[i].estado == 1 || avioes[i].estado == 2 || avioes[i].estado == 3)) {
            
            char msg[200];
            snprintf(msg, sizeof(msg), "PREEMPCAO: Aviao %d (DOM crítico) forçou liberação do aviao %d (INTL)", 
                     critical_aviao_id, avioes[i].id);
            log_msg(msg);
            
            avioes[i].tempo_inicio = time(NULL); 
            avioes[i].estado = 0; 
            
            int victim_id = avioes[i].id;
            
            pthread_mutex_unlock(&avioes_mutex); 
            
            pthread_mutex_lock(&stats_mutex);
            preempcoes_realizadas++;
            pthread_mutex_unlock(&stats_mutex);
            
            release_res(&torre, VOO_INTERNACIONAL, 1, victim_id);
            release_res(&pistas, VOO_INTERNACIONAL, 0, victim_id);
            release_res(&portoes, VOO_INTERNACIONAL, 0, victim_id);
            
            return victim_id;
        }
    }
    
    pthread_mutex_unlock(&avioes_mutex);
    return -1;
}

int force_preemption_by_id(int victim_id) {
    pthread_mutex_lock(&avioes_mutex);
    
    for (int i = 0; i < airplane_counter; i++) {
        if (avioes[i].id == victim_id &&
            (avioes[i].estado == 0 || avioes[i].estado == 1 || avioes[i].estado == 2)) {
            
            char msg[200];
            snprintf(msg, sizeof(msg), "RESOLUCAO DEADLOCK: Aviao %d (%s) forçado a liberar recursos", 
                     victim_id, avioes[i].type ? "INTL" : "DOM");
            log_msg(msg);
            
            avioes[i].tempo_inicio = time(NULL); 
            avioes[i].estado = 0; 
            
            release_res(&torre, avioes[i].type, 1, avioes[i].id);
            release_res(&pistas, avioes[i].type, 0, avioes[i].id);
            release_res(&portoes, avioes[i].type, 0, avioes[i].id);
            
            pthread_mutex_unlock(&avioes_mutex);
            return victim_id;
        }
    }
    
    pthread_mutex_unlock(&avioes_mutex);
    return -1;
}

int resolve_deadlock(int aviao1_id, int aviao2_id) {  
    airplane_t* aviao1 = NULL;
    airplane_t* aviao2 = NULL;
    
    for (int i = 0; i < airplane_counter; i++) {
        if (avioes[i].id == aviao1_id) aviao1 = &avioes[i];
        if (avioes[i].id == aviao2_id) aviao2 = &avioes[i];
    }
    
    if (aviao1 == NULL || aviao2 == NULL) return -1;
    

    airplane_t* victim;
    if (aviao1->tempo_inicio > aviao2->tempo_inicio) {
        victim = aviao1;
    } else if (aviao2->tempo_inicio > aviao1->tempo_inicio) {
        victim = aviao2;
    } else {
        victim = (aviao1->type == VOO_DOMESTICO) ? aviao1 : aviao2;
    }
    
    char msg[200];
    snprintf(msg, sizeof(msg), "ESCOLHA VITIMA: Aviao %d (%s, idade: %lds) escolhido como vítima entre %d e %d",
             victim->id, victim->type ? "INTL" : "DOM", 
             time(NULL) - victim->tempo_inicio, aviao1_id, aviao2_id);
    log_msg(msg);
    
    int result = force_preemption_by_id(victim->id);
    
    if (result != -1) {
        pthread_mutex_lock(&stats_mutex);
        deadlocks_resolvidos++;
        pthread_mutex_unlock(&stats_mutex);
    }
    
    return result;
}

void* aging_thread(void* arg __attribute__((unused))) {
    while (simulation_running) {
        sleep(5); 
        
        int critical_id = check_preemption_needed();
        if (critical_id != -1) {
            force_preemption(critical_id);
            remove_from_critical_list(critical_id);
        }
    }
    return NULL;
}

void add_resource_holder(int aviao_id, int recurso_tipo) {
    pthread_mutex_lock(&deadlock_mutex);
    
    resource_holder_t* new_holder = malloc(sizeof(resource_holder_t));
    new_holder->aviao_id = aviao_id;
    new_holder->recurso_tipo = recurso_tipo;
    
    if (recurso_tipo == 0) { 
        new_holder->next = pistas_holders;
        pistas_holders = new_holder;
    } else if (recurso_tipo == 1) { 
        new_holder->next = portoes_holders;
        portoes_holders = new_holder;
    } else if (recurso_tipo == 2) { 
        new_holder->next = torre_holders;
        torre_holders = new_holder;
    }
    
    pthread_mutex_unlock(&deadlock_mutex);
}

void remove_resource_holder(int aviao_id, int recurso_tipo) {
    pthread_mutex_lock(&deadlock_mutex);
    
    resource_holder_t** list_head;
    if (recurso_tipo == 0) list_head = &pistas_holders;
    else if (recurso_tipo == 1) list_head = &portoes_holders;
    else if (recurso_tipo == 2) list_head = &torre_holders;
    else {
        pthread_mutex_unlock(&deadlock_mutex);
        return;
    }
    
    resource_holder_t* current = *list_head;
    resource_holder_t* prev = NULL;
    
    while (current != NULL) {
        if (current->aviao_id == aviao_id) {
            if (prev == NULL) {
                *list_head = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&deadlock_mutex);
}

void add_waiting_thread(int aviao_id, int recurso_tipo) {
    pthread_mutex_lock(&deadlock_mutex);
    
    waiting_thread_t* new_waiting = malloc(sizeof(waiting_thread_t));
    new_waiting->aviao_id = aviao_id;
    new_waiting->recurso_tipo = recurso_tipo;
    new_waiting->tempo_espera = time(NULL);
    new_waiting->next = waiting_threads;
    waiting_threads = new_waiting;
    
    pthread_mutex_unlock(&deadlock_mutex);
}

void remove_waiting_thread(int aviao_id) {
    pthread_mutex_lock(&deadlock_mutex);
    
    waiting_thread_t* current = waiting_threads;
    waiting_thread_t* prev = NULL;
    
    while (current != NULL) {
        if (current->aviao_id == aviao_id) {
            if (prev == NULL) {
                waiting_threads = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&deadlock_mutex);
}

int detect_deadlock() {
    pthread_mutex_lock(&deadlock_mutex);
    
    waiting_thread_t* waiter = waiting_threads;
    
    while (waiter != NULL) {
        resource_holder_t* holders;
        
        if (waiter->recurso_tipo == 0) holders = pistas_holders;
        else if (waiter->recurso_tipo == 1) holders = portoes_holders;
        else if (waiter->recurso_tipo == 2) holders = torre_holders;
        else {
            waiter = waiter->next;
            continue;
        }
        
        resource_holder_t* holder = holders;
        while (holder != NULL) {
            waiting_thread_t* holder_waiting = waiting_threads;
            while (holder_waiting != NULL) {
                if (holder_waiting->aviao_id == holder->aviao_id) {
                    resource_holder_t* waiter_resources[3] = {pistas_holders, portoes_holders, torre_holders};
                    
                    for (int i = 0; i < 3; i++) {
                        resource_holder_t* wr = waiter_resources[i];
                        while (wr != NULL) {
                            if (wr->aviao_id == waiter->aviao_id && i == holder_waiting->recurso_tipo) {
                                char msg[200];
                                snprintf(msg, sizeof(msg), 
                                    "DEADLOCK DETECTADO: Aviao %d espera recurso %d (ocupado por %d), Aviao %d espera recurso %d (ocupado por %d)",
                                    waiter->aviao_id, waiter->recurso_tipo, holder->aviao_id,
                                    holder->aviao_id, holder_waiting->recurso_tipo, waiter->aviao_id);
                                log_msg(msg);
                                
                                pthread_mutex_lock(&stats_mutex);
                                deadlocks_detectados++;
                                pthread_mutex_unlock(&stats_mutex);
                                
                                pthread_mutex_unlock(&deadlock_mutex);
                                
                                resolve_deadlock(waiter->aviao_id, holder->aviao_id);
                                
                                return 1; 
                            }
                            wr = wr->next;
                        }
                    }
                    break;
                }
                holder_waiting = holder_waiting->next;
            }
            holder = holder->next;
        }
        waiter = waiter->next;
    }
    
    pthread_mutex_unlock(&deadlock_mutex);
    return 0; 
}

void* deadlock_detection_thread(void* arg __attribute__((unused))) {
    while (simulation_running) {
        sleep(3); 
        detect_deadlock();
    }
    return NULL;
}

void signal_handler(int sig __attribute__((unused))) {
    simulation_running = 0;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    signal(SIGINT, signal_handler);
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pistas") == 0 && i + 1 < argc) {
            num_pistas = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--portoes") == 0 && i + 1 < argc) {
            num_portoes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--torre") == 0 && i + 1 < argc) {
            capacidade_torre = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tempo") == 0 && i + 1 < argc) {
            tempo_sim = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--intervalo-min") == 0 && i + 1 < argc) {
            intervalo_min = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--intervalo-max") == 0 && i + 1 < argc) {
            intervalo_max = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--intervalo") == 0 && i + 2 < argc) {
            intervalo_min = atoi(argv[++i]);
            intervalo_max = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Uso: %s [opções]\n", argv[0]);
            printf("  --pistas N      Número de pistas (padrão: 3)\n");
            printf("  --portoes N     Número de portões (padrão: 5)\n");
            printf("  --torre N       Capacidade da torre - operações simultâneas (padrão: 2)\n");
            printf("  --tempo N       Tempo de simulação (padrão: 300)\n");
            printf("  --intervalo MIN MAX  Intervalo aleatório entre aviões em ms (padrão: 1000 3000)\n");
            printf("  --intervalo-min N    Intervalo mínimo em ms (padrão: 1000)\n");
            printf("  --intervalo-max N    Intervalo máximo em ms (padrão: 3000)\n");
            exit(0);
        }
    }
    
    if (intervalo_min >= intervalo_max) {
        printf("ERRO: Intervalo mínimo (%d) deve ser menor que máximo (%d)\n", 
               intervalo_min, intervalo_max);
        exit(1);
    }
    
    init_resource(&pistas, num_pistas, 0);
    init_resource(&portoes, num_portoes, 0);
    init_resource(&torre, capacidade_torre, 1); 
    start_time = time(NULL);
    
    log_msg("=== SIMULACAO INICIADA ===");
    char config_msg[200];
    snprintf(config_msg, sizeof(config_msg), 
             "CONFIGURACAO: Pistas=%d, Portoes=%d, Torre=%d, Tempo=%ds, Intervalo=%d-%dms", 
             num_pistas, num_portoes, capacidade_torre, tempo_sim, intervalo_min, intervalo_max);
    log_msg(config_msg);
    
    pthread_t monitor_tid, aging_tid, deadlock_tid;
    pthread_create(&monitor_tid, NULL, monitor_thread, NULL);
    pthread_create(&aging_tid, NULL, aging_thread, NULL);
    pthread_create(&deadlock_tid, NULL, deadlock_detection_thread, NULL);
    
    while (simulation_running && (time(NULL) - start_time) < tempo_sim) {
        if (airplane_counter < MAX_AVIOES) {
            pthread_mutex_lock(&avioes_mutex);
            airplane_t* plane = &avioes[airplane_counter];
            plane->id = airplane_counter++;
            plane->type = rand() % 2;
            pthread_mutex_unlock(&avioes_mutex);
            pthread_create(&plane->thread_id, NULL, airplane_thread, plane);
        }

        int intervalo_range = intervalo_max - intervalo_min;
        int intervalo_aleatorio = intervalo_min + (rand() % (intervalo_range + 1));
        usleep(intervalo_aleatorio * 1000); 
    }
    
    log_msg("=== TEMPO ESGOTADO - Aguardando avioes ativos ===");
    
    while (simulation_running) {
        pthread_mutex_lock(&stats_mutex);
        int avioes_ativos = ativos;
        pthread_mutex_unlock(&stats_mutex);
        
        if (avioes_ativos == 0) {
            log_msg("Todos os avioes finalizaram!");
            break;
        }
        sleep(2);
    }
    
    simulation_running = 0;
    
    pthread_cond_broadcast(&pistas.cond);
    pthread_cond_broadcast(&portoes.cond);
    pthread_cond_broadcast(&torre.cond);
    
    for (int i = 0; i < airplane_counter; i++) {
        pthread_join(avioes[i].thread_id, NULL);
    }
    pthread_join(monitor_tid, NULL);
    pthread_join(aging_tid, NULL);
    pthread_join(deadlock_tid, NULL);
    
    printf("\n==================================================================\n");
    printf("                    RELATORIO FINAL                               \n");
    printf("==================================================================\n");
    printf("CONFIGURACAO: Pistas=%d, Portoes=%d, Torre=%d, Tempo=%ds\n", 
           num_pistas, num_portoes, capacidade_torre, tempo_sim);
    printf("\nRESUMO GERAL:\n");
    printf("Total de avioes: %d\n", total_avioes);
    printf("├─ Domesticos: %d (%.1f%%)\n", domesticos, 
           total_avioes > 0 ? (float)domesticos/total_avioes*100 : 0);
    printf("└─ Internacionais: %d (%.1f%%)\n", internacionais,
           total_avioes > 0 ? (float)internacionais/total_avioes*100 : 0);
    printf("\nRESULTADOS:\n");
    printf("Sucessos: %d (%.1f%%)\n", sucessos, 
           total_avioes > 0 ? (float)sucessos/total_avioes*100 : 0);
    printf("Quedas: %d (%.1f%%)\n", quedas,
           total_avioes > 0 ? (float)quedas/total_avioes*100 : 0);
    printf("\nPROBLEMAS DETECTADOS:\n");
    printf("Alertas Criticos: %d\n", alertas_criticos);
    printf("Casos de Starvation: %d\n", starvation_casos);
    printf("Deadlocks Detectados: %d\n", deadlocks_detectados);
    printf("Deadlocks Resolvidos: %d\n", deadlocks_resolvidos);
    printf("Deadlocks Evitados (Backoff): %d\n", deadlocks_evitados);
    printf("Preempcoes Realizadas: %d\n", preempcoes_realizadas);
    printf("\nESTADO FINAL DOS AVIOES:\n");
    
    int sucessos_dom = 0, sucessos_int = 0, quedas_dom = 0, quedas_int = 0;
    for (int i = 0; i < airplane_counter; i++) {
        const char* estado_str;
        switch(avioes[i].estado) {
            case 3: estado_str = "SUCESSO"; break;
            case -1: estado_str = "QUEDA"; break;
            case 0: estado_str = "POUSO"; break;
            case 1: estado_str = "DESEMBARQUE"; break;
            case 2: estado_str = "DECOLAGEM"; break;
            default: estado_str = "DESCONHECIDO"; break;
        }
        
        if (avioes[i].estado == 3) {
            if (avioes[i].type == VOO_DOMESTICO) sucessos_dom++;
            else sucessos_int++;
        } else if (avioes[i].estado == -1) {
            if (avioes[i].type == VOO_DOMESTICO) quedas_dom++;
            else quedas_int++;
        }
        
        if (i < 10 || avioes[i].estado != 3) { 
            printf("Aviao %d (%s): %s\n", avioes[i].id, 
                   avioes[i].type ? "INTL" : "DOM", estado_str);
        }
    }
    
    printf("\nDETALHES POR TIPO:\n");
    printf("Domesticos - Sucessos: %d, Quedas: %d\n", sucessos_dom, quedas_dom);
    printf("Internacionais - Sucessos: %d, Quedas: %d\n", sucessos_int, quedas_int);
    printf("\nEFICIENCIA DO SISTEMA:\n");
    printf("Taxa de Sucesso: %.1f%%\n", 
           total_avioes > 0 ? (float)sucessos/total_avioes*100 : 0);
    printf("Taxa de Utilizacao (estimada): %.1f%%\n",
           total_avioes > 0 ? (float)sucessos/(tempo_sim/10.0)*100 : 0);
    printf("==================================================================\n");
    
    pthread_mutex_lock(&critical_mutex);
    critical_airplane_t* current = critical_list;
    while (current != NULL) {
        critical_airplane_t* next = current->next;
        free(current);
        current = next;
    }
    critical_list = NULL;
    pthread_mutex_unlock(&critical_mutex);
    
    pthread_mutex_lock(&deadlock_mutex);
    
    resource_holder_t* holder = pistas_holders;
    while (holder != NULL) {
        resource_holder_t* next = holder->next;
        free(holder);
        holder = next;
    }
    holder = portoes_holders;
    while (holder != NULL) {
        resource_holder_t* next = holder->next;
        free(holder);
        holder = next;
    }
    holder = torre_holders;
    while (holder != NULL) {
        resource_holder_t* next = holder->next;
        free(holder);
        holder = next;
    }
    
    waiting_thread_t* waiting = waiting_threads;
    while (waiting != NULL) {
        waiting_thread_t* next = waiting->next;
        free(waiting);
        waiting = next;
    }
    
    pthread_mutex_unlock(&deadlock_mutex);
    
    pthread_mutex_destroy(&pistas.mutex);
    pthread_cond_destroy(&pistas.cond);
    pthread_mutex_destroy(&portoes.mutex);
    pthread_cond_destroy(&portoes.cond);
    pthread_mutex_destroy(&torre.mutex);
    pthread_cond_destroy(&torre.cond);
    pthread_cond_destroy(&torre.cond_dom);
    pthread_mutex_destroy(&stats_mutex);
    pthread_mutex_destroy(&critical_mutex);
    pthread_mutex_destroy(&deadlock_mutex);
    
    return 0;
}
