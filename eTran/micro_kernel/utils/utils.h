#pragma once

static inline bool exec_cmd(const std::string &cmd, std::string &result, bool check_status = false)
{
    std::FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return false;

    char buffer[128];
    result.clear();
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        result += buffer;
    }

    const int status = pclose(pipe);
    return !check_status || status == 0;
}

static inline bool exec_cmd(const std::string &cmd)
{
    std::string unused_result;
    return exec_cmd(cmd, unused_result);
}
