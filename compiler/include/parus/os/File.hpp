// compiler/include/parus/os/File.hpp
#pragma once
#include <string>


namespace parus {

    /// @brief 파일을 열어서 내용을 문자열로 변환 (텍스트 모드)
    /// @details 내부에서 CRLF 정규화(\r\n -> \n, \r -> 제거) 수행
    bool open_file(const std::string& path, std::string& out_content, std::string& out_error);

    /// @brief 입력 경로를 “표시용/캐시용”으로 정규화
    /// @details OS별 경로 구분자, 절대/상대 처리, canonicalize 등을 포함
    std::string normalize_path(const std::string& path);

} // namespace parus