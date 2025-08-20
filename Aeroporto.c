#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

// Configurações
#define TIMEOUT_QUEDA 90
#define TEMPO_ALERTA 60
#define MAX_AVIOES 1000
#define VOO_DOMESTICO 0
#define VOO_INTERNACIONAL 1

// Estrutura unificada para recursos
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_cond_t cond_dom;  // Só para torre
    int available;
    int waiting_dom, waiting_int;
    time_t oldest_dom_time;
} resource_t;

// Estrutura do avião
typedef struct {
    int id, type;
    pthread_t thread_id;
    time_t tempo_inicio;
    int estado; // 0=pouso, 1=desembarque, 2=decolagem, 3=sucesso, -1=queda
} airplane_t;

// Variáveis globais
resource_t pistas, portoes, torre;
airplane_t avioes[MAX_AVIOES];
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
int total_avioes = 0, sucessos = 0, quedas = 0, ativos = 0;
int domesticos = 0, internacionais = 0;
int alertas_criticos = 0, deadlocks_detectados = 0, starvation_casos = 0;
int num_pistas = 3, num_portoes = 5, capacidade_torre = 2, tempo_sim = 300;
int simulation_running = 1, airplane_counter = 0;
time_t start_time;

// Funções
void init_resource(resource_t* res, int capacity, int is_torre);
int acquire_res(resource_t* res, int type, int timeout, int is_torre, int aviao_id, time_t tempo_inicio);
void release_res(resource_t* res, int type, int is_torre);
int realizar_pouso(airplane_t* plane);
int realizar_desembarque(airplane_t* plane);
int realizar_decolagem(airplane_t* plane);
void* airplane_thread(void* arg);
void* monitor_thread(void* arg);
void log_msg(const char* msg);
void update_stats(int status, int type);

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
    
    pthread_mutex_lock(&res->mutex);
    
    while (res->available <= 0 && simulation_running) {
        // Verifica tempo de vida do avião
        time_t tempo_vida = time(NULL) - tempo_inicio;
        
        // Alerta crítico aos 60s
        if (tempo_vida >= TEMPO_ALERTA && !alerta_enviado) {
            char msg[100];
            snprintf(msg, sizeof(msg), "ALERTA CRITICO: Aviao %d (%s) esperando %lds", 
                     aviao_id, type ? "INTL" : "DOM", tempo_vida);
            log_msg(msg);
            pthread_mutex_lock(&stats_mutex);
            alertas_criticos++;
            pthread_mutex_unlock(&stats_mutex);
            alerta_enviado = 1;
        }
        
        // Starvation aos 90s
        if (tempo_vida >= TIMEOUT_QUEDA) {
            char msg[100];
            snprintf(msg, sizeof(msg), "STARVATION: Aviao %d (%s) caiu apos %lds", 
                     aviao_id, type ? "INTL" : "DOM", tempo_vida);
            log_msg(msg);
            pthread_mutex_lock(&stats_mutex);
            starvation_casos++;
            pthread_mutex_unlock(&stats_mutex);
            pthread_mutex_unlock(&res->mutex);
            return -1;
        }
        
        if (pthread_cond_timedwait(&res->cond, &res->mutex, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&res->mutex);
            return -1;
        }
    }
    
    if (!simulation_running || res->available <= 0) {
        pthread_mutex_unlock(&res->mutex);
        return -1;
    }
    
    res->available--;
    pthread_mutex_unlock(&res->mutex);
    return 0;
}

void release_res(resource_t* res, int type, int is_torre) {
    pthread_mutex_lock(&res->mutex);
    res->available++;
    pthread_cond_signal(&res->cond);
    pthread_mutex_unlock(&res->mutex);
}

int realizar_pouso(airplane_t* plane) {
    if (plane->type == VOO_INTERNACIONAL) {
        if (acquire_res(&pistas, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) return -1;
        if (acquire_res(&torre, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&pistas, plane->type, 0);
            return -1;
        }
    } else {
        if (acquire_res(&torre, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) return -1;
        if (acquire_res(&pistas, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&torre, plane->type, 0);
            return -1;
        }
    }
    
    char msg[100];
    snprintf(msg, sizeof(msg), "Aviao %d: POUSANDO", plane->id);
    log_msg(msg);
    sleep(1 + rand() % 2);
    
    release_res(&pistas, plane->type, 0);
    release_res(&torre, plane->type, 0);
    return 0;
}

int realizar_desembarque(airplane_t* plane) {
    if (plane->type == VOO_INTERNACIONAL) {
        if (acquire_res(&portoes, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) return -1;
        if (acquire_res(&torre, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&portoes, plane->type, 0);
            return -1;
        }
    } else {
        if (acquire_res(&torre, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) return -1;
        if (acquire_res(&portoes, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&torre, plane->type, 0);
            return -1;
        }
    }
    
    char msg[100];
    snprintf(msg, sizeof(msg), "Aviao %d: DESEMBARCANDO", plane->id);
    log_msg(msg);
    sleep(2 + rand() % 3);
    release_res(&torre, plane->type, 0);
    sleep(1);
    return 0;
}

int realizar_decolagem(airplane_t* plane) {
    if (plane->type == VOO_INTERNACIONAL) {
        if (acquire_res(&pistas, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&portoes, plane->type, 0);
            return -1;
        }
        if (acquire_res(&torre, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&pistas, plane->type, 0);
            release_res(&portoes, plane->type, 0);
            return -1;
        }
    } else {
        if (acquire_res(&torre, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&portoes, plane->type, 0);
            return -1;
        }
        if (acquire_res(&pistas, plane->type, TIMEOUT_QUEDA, 0, plane->id, plane->tempo_inicio) != 0) {
            release_res(&torre, plane->type, 0);
            release_res(&portoes, plane->type, 0);
            return -1;
        }
    }
    
    char msg[100];
    snprintf(msg, sizeof(msg), "Aviao %d: DECOLANDO", plane->id);
    log_msg(msg);
    sleep(1 + rand() % 2);
    
    release_res(&pistas, plane->type, 0);
    release_res(&torre, plane->type, 0);
    release_res(&portoes, plane->type, 0);
    return 0;
}


void* airplane_thread(void* arg) {
    airplane_t* plane = (airplane_t*)arg;
    char msg[100];
    
    plane->tempo_inicio = time(NULL);
    plane->estado = 0;
    
    pthread_mutex_lock(&stats_mutex);
    ativos++;
    pthread_mutex_unlock(&stats_mutex);
    
    snprintf(msg, sizeof(msg), "Aviao %d (%s): Iniciando", 
             plane->id, plane->type ? "INTL" : "DOM");
    log_msg(msg);
    
    // POUSO
    plane->estado = 0;
    if (realizar_pouso(plane) != 0) {
        plane->estado = -1;
        time_t tempo_total = time(NULL) - plane->tempo_inicio;
        snprintf(msg, sizeof(msg), "Aviao %d: QUEDA (tempo total: %lds)", plane->id, tempo_total);
        log_msg(msg);
        update_stats(-1, plane->type);
        pthread_mutex_lock(&stats_mutex);
        ativos--;
        pthread_mutex_unlock(&stats_mutex);
        return NULL;
    }
    
    // DESEMBARQUE
    plane->estado = 1;
    if (realizar_desembarque(plane) != 0) {
        plane->estado = -1;
        time_t tempo_total = time(NULL) - plane->tempo_inicio;
        snprintf(msg, sizeof(msg), "Aviao %d: QUEDA (tempo total: %lds)", plane->id, tempo_total);
        log_msg(msg);
        update_stats(-1, plane->type);
        pthread_mutex_lock(&stats_mutex);
        ativos--;
        pthread_mutex_unlock(&stats_mutex);
        return NULL;
    }
    
    // DECOLAGEM
    plane->estado = 2;
    if (realizar_decolagem(plane) != 0) {
        plane->estado = -1;
        time_t tempo_total = time(NULL) - plane->tempo_inicio;
        snprintf(msg, sizeof(msg), "Aviao %d: QUEDA (tempo total: %lds)", plane->id, tempo_total);
        log_msg(msg);
        update_stats(-1, plane->type);
        pthread_mutex_lock(&stats_mutex);
        ativos--;
        pthread_mutex_unlock(&stats_mutex);
        return NULL;
    }
    
    // SUCESSO
    plane->estado = 3;
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
        printf("Alertas: %d | Starvation: %d | Deadlocks: %d\n", 
               alertas_criticos, starvation_casos, deadlocks_detectados);
        
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

void signal_handler(int sig __attribute__((unused))) {
    simulation_running = 0;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    signal(SIGINT, signal_handler);
    
    // Parse argumentos
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pistas") == 0 && i + 1 < argc) {
            num_pistas = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--portoes") == 0 && i + 1 < argc) {
            num_portoes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--torre") == 0 && i + 1 < argc) {
            capacidade_torre = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tempo") == 0 && i + 1 < argc) {
            tempo_sim = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Uso: %s [opções]\n", argv[0]);
            printf("  --pistas N      Número de pistas (padrão: 3)\n");
            printf("  --portoes N     Número de portões (padrão: 5)\n");
            printf("  --torre N       Capacidade da torre (padrão: 2)\n");
            printf("  --tempo N       Tempo de simulação (padrão: 300)\n");
            exit(0);
        }
    }
    
    // Inicializar recursos
    init_resource(&pistas, num_pistas, 0);
    init_resource(&portoes, num_portoes, 0);
    init_resource(&torre, capacidade_torre, 0);
    start_time = time(NULL);
    
    log_msg("=== SIMULACAO INICIADA ===");
    
    pthread_t monitor_tid;
    pthread_create(&monitor_tid, NULL, monitor_thread, NULL);
    
    // Loop principal
    while (simulation_running && (time(NULL) - start_time) < tempo_sim) {
        if (airplane_counter < MAX_AVIOES) {
            airplane_t* plane = &avioes[airplane_counter];
            plane->id = airplane_counter++;
            plane->type = rand() % 2;
            pthread_create(&plane->thread_id, NULL, airplane_thread, plane);
        }
        sleep(2 + rand() % 3);
    }
    
    log_msg("=== TEMPO ESGOTADO - Aguardando avioes ativos ===");
    
    // Aguardar aviões ativos
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
    
    // Acordar threads e aguardar
    pthread_cond_broadcast(&pistas.cond);
    pthread_cond_broadcast(&portoes.cond);
    pthread_cond_broadcast(&torre.cond);
    
    for (int i = 0; i < airplane_counter; i++) {
        pthread_join(avioes[i].thread_id, NULL);
    }
    pthread_join(monitor_tid, NULL);
    
    // Relatório final detalhado
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
        
        if (i < 10 || avioes[i].estado != 3) { // Mostra primeiros 10 ou os que falharam
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
    
    // Cleanup
    pthread_mutex_destroy(&pistas.mutex);
    pthread_cond_destroy(&pistas.cond);
    pthread_mutex_destroy(&portoes.mutex);
    pthread_cond_destroy(&portoes.cond);
    pthread_mutex_destroy(&torre.mutex);
    pthread_cond_destroy(&torre.cond);
    pthread_mutex_destroy(&stats_mutex);
    
    return 0;
}
