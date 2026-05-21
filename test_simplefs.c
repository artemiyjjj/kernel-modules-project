#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define SIMPLEFS_IOC_MAGIC 'f'
#define SIMPLEFS_IOC_RESET_FILES   _IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOC_WIPE_FS       _IO(SIMPLEFS_IOC_MAGIC, 2)
#define SIMPLEFS_IOC_GET_HASHES    _IOR(SIMPLEFS_IOC_MAGIC, 3, char*)
#define SIMPLEFS_IOC_GET_MAPPING   _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_mapping_args)

struct simplefs_mapping_args {
    int fd;
    uint32_t max_sectors;
    uint32_t num_sectors;
    uint64_t *sectors;
};

#define TOTAL_SECTORS 100 

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Использование: %s <путь_к_mnt> <команда> [доп_аргументы]\n", argv);
        fprintf(stderr, "Команды:\n  reset  - обнулить файлы\n  wipe   - уничтожить ФС\n  hash   - взять CRC32 секторов\n  stress - стресс-тест I/O всех файлов\n");
        return 1;
    }

    char *mount_path = argv[1];
    char *cmd = argv[2];

    int fd = open(mount_path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        perror("Ошибка открытия точки монтирования");
        return 1;
    }

    if (strcmp(cmd, "reset") == 0) {
        if (ioctl(fd, SIMPLEFS_IOC_RESET_FILES) < 0) perror("ioctl RESET_FILES");
        else printf("Успешно: Все файлы-сектора обнулены.\n");
    } 
    else if (strcmp(cmd, "wipe") == 0) {
        if (ioctl(fd, SIMPLEFS_IOC_WIPE_FS) < 0) perror("ioctl WIPE_FS");
        else printf("Успешно: Структура суперблоков стерта.\n");
    } 
    else if (strcmp(cmd, "hash") == 0) {
        uint32_t hashes[TOTAL_SECTORS];
        if (ioctl(fd, SIMPLEFS_IOC_GET_HASHES, hashes) < 0) {
            perror("ioctl GET_HASHES");
        } else {
            for (int i = 0; i < TOTAL_SECTORS; i++) {
                printf("Файл %d (Сектор %d): CRC32 = 0x%08X\n", i, i + 2, hashes[i]);
            }
        }
    } 
    else if (strcmp(cmd, "stress") == 0) {
        srand(time(NULL));
        printf("Запуск автоматического стресс-теста для SimpleFS...\n");

        for (int i = 0; i < TOTAL_SECTORS; i++) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/file_%d", mount_path, i);

            /* 1. Генерируем случайные данные */
            int secret_number = rand();
            char write_buf[64];
            int write_len = snprintf(write_buf, sizeof(write_buf), "%d\0", secret_number);

            /* 2. Запись в файл */
            int f_fd = open(filepath, O_RDWR);
            if (f_fd < 0) {
                printf("Файл %s пропущен (не инициализирован в суперблоке)\n", filepath);
                continue;
            }

            if (write(f_fd, write_buf, write_len) != write_len) {
                perror("Ошибка записи");
                close(f_fd);
                break;
            }

            /* 3. Вызываем ioctl маппинга, чтобы подтвердить сектор */
            uint64_t sector_num = 0;
            struct simplefs_mapping_args margs = { .fd = f_fd, .max_sectors = 1, .sectors = &sector_num };
            if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &margs) == 0) {
                printf("[Файл %d] записан -> физ. сектор диска: %llu... ", i, (unsigned long long)sector_num);
            }

            /* Сбрасываем кэш конкретного дескриптора и закрываем */
            fsync(f_fd);
            close(f_fd);

            /* 4. Чтение для верификации */
            f_fd = open(filepath, O_RDONLY);
            char read_buf[64] = {0};
            if (read(f_fd, read_buf, write_len) < 0) {
                perror("Ошибка чтения");
                close(f_fd);
                break;
            }
            close(f_fd);

            /* 5. Сверка результатов */
            if (strcmp(write_buf, read_buf) == 0) {
                printf("Проверка пройдена успешно (прочитано: %s", read_buf);
            } else {
                printf("КРИТИЧЕСКАЯ ОШИБКА: Данные не сошлись! Ожидалось %s, получили %s\n", write_buf, read_buf);
                break;
            }
        }
        printf("Стресс-тест успешно завершен для всех секторов!\n");
    } 
    else {
        fprintf(stderr, "Неизвестная команда: %s\n", cmd);
    }

    close(fd);
    return 0;
}
