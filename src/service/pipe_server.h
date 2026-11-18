#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace sovereign::service {

// Однопоточный echo-сервер именованного канала. Принимает один запрос за
// подключение, отвечает через handler и закрывает соединение. Приём нового
// клиента (ConnectNamedPipe) выполняется через overlapped I/O, чтобы
// std::stop_token мог прервать ожидание — без этого служба не смогла бы
// остановиться, пока не подключится клиент.
class PipeServer {
 public:
  using RequestHandler = std::function<std::string(const std::string& request)>;

  // `trustedClientImage`: the one program that may use the pipe besides
  // elevated processes (pipe_access.h); none - anyone the DACL lets in.
  PipeServer(std::wstring pipeName, RequestHandler handler, std::optional<std::wstring> trustedClientImage = std::nullopt);

  // Блокирует вызывающий поток до stopToken.stop_requested().
  void Run(const std::stop_token& stopToken);

 private:
  std::wstring pipeName_;
  RequestHandler handler_;
  std::optional<std::wstring> trustedClientImage_;
};

}  // namespace sovereign::service
