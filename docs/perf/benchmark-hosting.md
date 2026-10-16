# Where to run formal benchmarks

Formal, fair benchmarks of strata against Lucene, Tantivy, Xapian, and the KV systems
(RocksDB) need dedicated bare-metal machines: Linux and FreeBSD first, macOS arm64 so
developers can reproduce numbers on their own Macs. This compares the options and what
each costs. Prices are on-demand list prices found in October 2026, mostly through
third-party price trackers; check them against the providers' pages before buying.

## What a campaign needs

One full campaign per platform:

| Step | Time |
|---|---|
| Provision, install toolchains, build every engine | 1–2 h |
| Index the 5M-document corpus with each engine (strata, Lucene ×2, Tantivy, Xapian), 5 trials for the indexing rows | 4–5 h |
| Query trials: 5 engines × 5 trials × 2 query sets | 3–4 h |
| Memory, disk, and KV rows | 1–2 h |
| **Total** | **≈ 10–12 h** |

Platforms: Linux x86_64, FreeBSD x86_64, Linux arm64, macOS arm64. Local NVMe matters
for the indexing and cold-cache rows; network block storage (EBS, Persistent Disk) adds
its own noise, so prefer instances with local NVMe.

## Options

### A. AWS bare metal, on demand (recommended for formal, published runs)

| Platform | Instance | Price | 12 h campaign |
|---|---|---:|---:|
| Linux x86_64 | `c7i.metal-24xl` (96 vCPU) | $4.28/h | ≈ $51 |
| FreeBSD x86_64 | same type, official FreeBSD AMI | $4.28/h | ≈ $51 |
| Linux arm64 | `c8g.metal-24xl` (Graviton4) | ≈ $3.83/h | ≈ $46 |
| macOS arm64 | `mac-m4.metal` (Dedicated Host, 24 h minimum) | $1.23/h | ≈ $30 |

- **Per campaign:** about **$180–$250** with storage and transfer. Four campaigns a year
  is about $1,000.
- **For:** anyone can rent the same machine types and reproduce the numbers. FreeBSD has
  official AMIs. One provider covers all four platforms, including macOS.
- **Against:** the `c7i`/`c8g` metal sizes have no local NVMe, so indexing reads and
  writes EBS. For storage rows, use a `d` variant with instance NVMe (`c6id`, `m7gd`, or
  `i4i` metal) at a higher rate. Mac hosts bill at least 24 hours per allocation.

### B. Google Cloud bare metal

- **Instance:** `c3-highcpu-192-metal` at about $8.67/h on demand (spot about $1.3–2.2/h,
  but spot can be reclaimed mid-trial). About $105 per 12-hour campaign on one platform.
- **Against:** no macOS. FreeBSD on GCE is community-supported, not official. The
  smallest metal shape is 192 vCPUs, more than these benchmarks use.
- **Verdict:** no advantage over AWS for this.

### C. Hetzner dedicated servers (recommended for continuous runs)

- **Instance:** AX42 (AMD Ryzen 7 PRO, 64 GB, 2 × NVMe) at €97.30/month plus a €49 setup
  fee, after Hetzner's June 2026 price change. Exclusive hardware, local NVMe, billed
  monthly.
- **Platforms:** Linux is supported. FreeBSD is a manual install from the rescue system;
  I couldn't confirm official support, so verify before ordering. Hetzner's arm64
  dedicated line (Ampere) is an option to check. No macOS.
- **Cost:** two boxes (Linux and FreeBSD) at about **€200/month**, about €2,400 a year.
- **For:** always on and never shared. Nightly scheduled runs and the quick A/Bs move
  off the development host, which ends today's problem of benchmarks blocking the host
  and its agents.
- **Against:** a less common machine type for outside reproduction, and EU locations
  (latency doesn't matter for local benchmarks).

### D. macOS arm64 alternatives

| Option | Cost | Note |
|---|---|---|
| AWS `mac-m4.metal` | $1.23/h, 24 h minimum (≈ $30 a run) | Same provider as A |
| Scaleway Mac mini M4 | €0.22/h (24 h minimum) or €149/month | EU; M4 Pro available |
| Own Mac mini M4 | one-time, about $600 | Cheapest after about 5 months; must stay quiet and dedicated |

The M1 used before is retired (Workspace ONE), so any macOS option is new.

## Recommendation

Two tiers, matching the benchmark policy (quick A/Bs by default, full runs at gates):

1. **Continuous tier:** two Hetzner AX42-class boxes (Linux, FreeBSD), about €200/month.
   They run the nightly quick A/Bs and every gate's full five-trial runs, scheduled,
   off the development host.
2. **Formal tier:** AWS on demand for published results: Linux x86_64 and FreeBSD on
   metal with local NVMe, Graviton for arm64, `mac-m4.metal` for macOS. About
   $200–300 per campaign, run at phase gates and releases, with the scripts and machine
   types published so others can repeat them.

Before committing: confirm FreeBSD on the chosen Hetzner model, and price the AWS `d`
metal variants (local NVMe), since those set the storage rows.

## Sources

- [AWS EC2 Mac pricing (macyou.co)](https://macyou.co/blog/aws-ec2-mac-pricing-explained), [Vantage: mac2-m2pro.metal](https://instances.vantage.sh/aws/ec2/mac2-m2pro.metal), [AWS: EC2 Mac instances](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-mac-instances.html)
- [Vantage: c7i.metal-24xl](https://instances.vantage.sh/aws/ec2/c7i.metal-24xl), [Spare Cores: c7i.metal-24xl](https://sparecores.com/server/aws/c7i.metal-24xl), [cloudprice: c8g/x8g](https://cloudprice.net/aws/ec2/compare/c7i.metal-24xl-vs-x8g.metal-24xl)
- [Hetzner price adjustment, June 2026](https://docs.hetzner.com/general/infrastructure-and-availability/price-adjustment/)
- [Scaleway Apple silicon pricing](https://www.scaleway.com/en/pricing/apple-silicon/)
- [Spare Cores: c3-highcpu-192-metal](https://sparecores.com/server/gcp/c3-highcpu-192-metal), [Google Cloud pricing](https://cloud.google.com/products/compute/pricing/general-purpose)
