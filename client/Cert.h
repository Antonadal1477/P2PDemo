#ifndef _P2PTRANSFER_H_
#define _P2PTRANSFER_H_

#include <string>

struct CertificatePair {
    std::string certPem;
    std::string keyPem;
};

CertificatePair generate_ecdsa_certificate();

#endif
