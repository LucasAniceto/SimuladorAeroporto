# Sistema de Controle de Tráfego Aéreo

Simulador de controle de tráfego aéreo desenvolvido em C com pthread para a disciplina de Sistemas Operacionais.

## Descrição

Este projeto simula um aeroporto internacional com recursos limitados (pistas, portões e torre de controle) onde aviões (threads) competem por recursos seguindo diferentes ordens de aquisição, criando cenários propícios para deadlocks e starvation.

**Características principais:**
- Prevenção e resolução de deadlocks
- Sistema de aging para evitar starvation  
- Priorização de voos internacionais
- Monitoramento em tempo real
- Métricas detalhadas de performance

## Compilação

```bash
gcc -o aeroporto aeroporto.c -lpthread -Wall -Wextra
```

## Execução

### Execução básica
```bash
./aeroporto
```

### Parâmetros disponíveis
```bash
./aeroporto --help
```

### Exemplos de uso
```bash
# Configuração padrão (5 minutos)
./aeroporto

# Aeroporto pequeno com poucos recursos
./aeroporto --pistas 2 --portoes 3 --torre 1 --tempo 180

# Aeroporto grande com mais recursos  
./aeroporto --pistas 5 --portoes 8 --torre 3 --tempo 600

# Teste de stress (muitos aviões)
./aeroporto --intervalo 200 800 --tempo 300
```

## Parâmetros de Configuração

| Parâmetro | Descrição | Padrão |
|-----------|-----------|--------|
| `--pistas N` | Número de pistas | 3 |
| `--portoes N` | Número de portões | 5 |
| `--torre N` | Capacidade da torre | 2 |
| `--tempo N` | Duração da simulação (segundos) | 300 |
| `--intervalo MIN MAX` | Intervalo entre aviões (ms) | 1000 3000 |

## Saída do Sistema

O sistema exibe:
- **Logs em tempo real** de todas as operações
- **Status periódico** com estatísticas atualizadas
- **Relatório final** com métricas consolidadas

### Métricas principais:
- **DL Det:** Deadlocks detectados
- **DL Res:** Deadlocks resolvidos  
- **DL Evit:** Deadlocks evitados (backoff)
- **Preempções:** Intervenções do sistema de aging
- **Starvation:** Casos de timeout (90s)

## Requisitos

- **SO:** Linux/Unix ou Windows com ambiente POSIX
- **Compilador:** GCC com suporte a pthread
- **Bibliotecas:** pthread, stdlib, stdio, unistd, time

## Autores

- Lucas Aniceto
- Rodrigo Santos

**Disciplina:** Sistemas Operacionais  
**Professor:** Rafael Burlamaqui Amaral  
**Universidade Federal de Pelotas**
