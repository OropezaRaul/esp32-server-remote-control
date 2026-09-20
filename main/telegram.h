#pragma once

#include <esp_err.h>
#include <stdbool.h>

void telegram_init(void);
void telegram_poll_task(void *arg);

esp_err_t telegram_send_text(int64_t chat_id, const char *text);

// Envía `text` a todos los chat IDs autorizados. Usado por automation.c
// para notificar acciones de la automatización. Si no hay chat IDs
// autorizados, retorna ESP_OK sin hacer nada.
esp_err_t telegram_broadcast(const char *text);

// Persiste g_last_update_id en NVS para que sobreviva a reinicios
// y Telegram nunca re-entregue mensajes antiguos en el próximo boot.
esp_err_t telegram_save_last_uid(void);

// Envía un documento/archivo a un chat_id vía sendDocument (multipart/form-data).
esp_err_t telegram_send_document(int64_t chat_id, const char *filename,
                                const char *file_content, size_t file_size,
                                const char *caption);

// Envía un documento a todos los chats autorizados.
esp_err_t telegram_broadcast_document(const char *filename,
                                      const char *file_content, size_t file_size,
                                      const char *caption);