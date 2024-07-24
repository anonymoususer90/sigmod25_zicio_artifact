import time, threading
import os, random
from datetime import datetime

# procedures of TPC-H queries
procs = [
"""
Q0
""",
"""
create procedure Q1(
    m_days int
)
begin    
    
select
    l_returnflag,
    l_linestatus,
    sum(l_quantity) as sum_qty,
    sum(l_extendedprice) as sum_base_price,
    sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
    sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
    avg(l_quantity) as avg_qty,
    avg(l_extendedprice) as avg_price,
    avg(l_discount) as avg_disc,
    count(*) as count_order
from
    lineitem
where
    l_shipdate <= date '1998-12-01' - interval m_days day
group by
    l_returnflag,
    l_linestatus
order by
    l_returnflag,
    l_linestatus;

end
""",
"""
create procedure Q2(
    m_size int,
    m_type varchar(25),
    m_region varchar(25)
)
begin

select
    s_acctbal,
    s_name,
    n_name,
    p_partkey,
    p_mfgr,
    s_address,
    s_phone,
    s_comment
from
    part,
    supplier,
    partsupp,
    nation,
    region
where
    p_partkey = ps_partkey
    and s_suppkey = ps_suppkey
    and p_size = m_size
    and p_type like concat('%', m_type)
    and s_nationkey = n_nationkey
    and n_regionkey = r_regionkey
    and r_name = m_region
    and ps_supplycost = (
        select
            min(ps_supplycost)
        from
            partsupp, 
            supplier,
            nation, 
            region
        where
            p_partkey = ps_partkey
            and s_suppkey = ps_suppkey
            and s_nationkey = n_nationkey
            and n_regionkey = r_regionkey
            and r_name = m_region
    )
order by
    s_acctbal desc,
    n_name,
    s_name,
    p_partkey
limit 100;

end
""",
"""
create procedure Q3(
    m_segment char(10),
    m_date date
)
begin

select
    l_orderkey,
    sum(l_extendedprice * (1 - l_discount)) as revenue,
    o_orderdate,
    o_shippriority
from
    customer,
    orders,
    lineitem
where
    c_mktsegment = m_segment
    and c_custkey = o_custkey
    and l_orderkey = o_orderkey
    and o_orderdate < m_date
    and l_shipdate > m_date
group by
    l_orderkey,
    o_orderdate,
    o_shippriority
order by
    revenue desc,
    o_orderdate
limit 10;

end
""",
"""
create procedure Q4(
    m_date date
)

select
    o_orderpriority,
    count(*) as order_count
from
    orders
where
    o_orderdate >= m_date
    and o_orderdate < m_date + interval '3' month
    and exists (
        select
            *
        from
            lineitem
        where
            l_orderkey = o_orderkey
            and l_commitdate < l_receiptdate
    )
group by
    o_orderpriority
order by
    o_orderpriority;

end
""",
"""
create procedure Q5(
    m_region varchar(25),
    m_date date
)
begin

select
    n_name,
    sum(l_extendedprice * (1 - l_discount)) as revenue
from
    customer,
    orders,
    lineitem,
    supplier,
    nation,
    region
where
    c_custkey = o_custkey
    and l_orderkey = o_orderkey
    and l_suppkey = s_suppkey
    and c_nationkey = s_nationkey
    and s_nationkey = n_nationkey
    and n_regionkey = r_regionkey
    and r_name = m_region
    and o_orderdate >= m_date
    and o_orderdate < m_date + interval '1' year
group by
    n_name
order by
    revenue desc;

end
""",
"""
create procedure Q6(
    m_date date,
    m_discount decimal,
    m_quantity int
)
begin

select
    sum(l_extendedprice*l_discount) as revenue
from
    lineitem
where
    l_shipdate >= m_date
    and l_shipdate < m_date + interval '1' year
    and l_discount between m_discount - 0.01 and m_discount + 0.01
    and l_quantity < m_quantity;

end
""",
"""
create procedure Q7(
    m_nation1 char(25),
    m_nation2 char(25)
)
begin

select
    supp_nation,
    cust_nation,
    l_year, sum(volume) as revenue
from 
    (
        select
            n1.n_name as supp_nation,
            n2.n_name as cust_nation,
            extract(year from l_shipdate) as l_year,
            l_extendedprice * (1 - l_discount) as volume
        from
            supplier,
            lineitem,
            orders,
            customer,
            nation n1,
            nation n2
        where
            s_suppkey = l_suppkey
            and o_orderkey = l_orderkey
            and c_custkey = o_custkey
            and s_nationkey = n1.n_nationkey
            and c_nationkey = n2.n_nationkey
            and (
                (n1.n_name = m_nation1 and n2.n_name = m_nation2)
                or (n1.n_name = m_nation2 and n2.n_name = m_nation1)
            )
            and l_shipdate between date '1995-01-01' and date '1996-12-31'
    ) as shipping
group by
    supp_nation,
    cust_nation,
    l_year
order by
    supp_nation,
    cust_nation,
    l_year;

end
""",
"""
create procedure Q8(
    m_nation char(25),
    m_region varchar(25),
    m_type varchar(25)
)
begin

select
    o_year,
    sum(case
        when nation = m_nation
        then volume
        else 0
    end) / sum(volume) as mkt_share
from 
    (
        select
            extract(year from o_orderdate) as o_year,
            l_extendedprice * (1-l_discount) as volume,
            n2.n_name as nation
        from
            part,
            supplier,
            lineitem,
            orders,
            customer,
            nation n1,
            nation n2,
            region
        where
            p_partkey = l_partkey
            and s_suppkey = l_suppkey
            and l_orderkey = o_orderkey
            and o_custkey = c_custkey
            and c_nationkey = n1.n_nationkey
            and n1.n_regionkey = r_regionkey
            and r_name = m_region
            and s_nationkey = n2.n_nationkey
            and o_orderdate between date '1995-01-01' and date '1996-12-31'
            and p_type = m_type
    ) as all_nations
group by
    o_year
order by
    o_year;

end
""",
"""
create procedure Q9(
    m_color varchar(55)
)
begin

select
    nation,
    o_year,
    sum(amount) as sum_profit
from 
    (
        select
            n_name as nation,
            extract(year from o_orderdate) as o_year,
            l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity as amount
        from
            part,
            supplier,
            lineitem,
            partsupp,
            orders,
            nation
        where
            s_suppkey = l_suppkey
            and ps_suppkey = l_suppkey
            and ps_partkey = l_partkey
            and p_partkey = l_partkey
            and o_orderkey = l_orderkey
            and s_nationkey = n_nationkey
            and p_name like concat('%', m_color, '%')
    ) as profit
group by
    nation,
    o_year
order by
    nation,
    o_year desc;

end
""",
"""
create procedure Q10(
    m_date date
)
begin

select
    c_custkey,
    c_name,
    sum(l_extendedprice * (1 - l_discount)) as revenue,
    c_acctbal,
    n_name,
    c_address,
    c_phone,
    c_comment
from
    customer,
    orders,
    lineitem,
    nation
where
    c_custkey = o_custkey
    and l_orderkey = o_orderkey
    and o_orderdate >= m_date
    and o_orderdate < m_date + interval '3' month
    and l_returnflag = 'R'
    and c_nationkey = n_nationkey
group by
    c_custkey,
    c_name,
    c_acctbal,
    c_phone,
    n_name,
    c_address,
    c_comment
order by
    revenue desc
limit 20;

end
""",
"""
create procedure Q11(
    m_nation char(25),
    m_fraction float
)
begin

select
    ps_partkey,
    sum(ps_supplycost * ps_availqty) as value
from
    partsupp,
    supplier,
    nation
where
    ps_suppkey = s_suppkey
    and s_nationkey = n_nationkey
    and n_name = m_nation
group by
    ps_partkey having
        sum(ps_supplycost * ps_availqty) > (
            select
                sum(ps_supplycost * ps_availqty) * m_fraction
            from
                partsupp,
                supplier,
                nation
            where
                ps_suppkey = s_suppkey
                and s_nationkey = n_nationkey
                and n_name = m_nation
        )
order by
    value desc;

end
""",
"""
create procedure Q12(
    m_shipmode1 char(10),
    m_shipmode2 char(10),
    m_date date
)
begin

select
    l_shipmode,
    sum(case
        when o_orderpriority ='1-URGENT'
            or o_orderpriority ='2-HIGH'
            then 1
        else 0
    end) as high_line_count,
    sum(case
        when o_orderpriority <> '1-URGENT'
            and o_orderpriority <> '2-HIGH'
            then 1
        else 0
    end) as low_line_count
from
    orders,
    lineitem
where
    o_orderkey = l_orderkey
    and l_shipmode in (m_shipmode1, m_shipmode2)
    and l_commitdate < l_receiptdate
    and l_shipdate < l_commitdate
    and l_receiptdate >= m_date
    and l_receiptdate < m_date + interval '1' year
group by
    l_shipmode
order by
    l_shipmode;

end
""",
"""
create procedure Q13(
    m_word1 varchar(16),
    m_word2 varchar(16)
)
begin

select
    c_count, count(*) as custdist
from 
    (
        select
            c_custkey,
            count(o_orderkey)
        from
            customer left outer join orders on
                c_custkey = o_custkey
                and o_comment not like concat('%', m_word1, '%', m_word2, '%')
        group by
            c_custkey
    )as c_orders (c_custkey, c_count)
group by
    c_count
order by
    custdist desc,
    c_count desc;

end
""",
"""
create procedure Q14(
    m_date date
)
begin

select
    100.00 * sum(case
        when p_type like 'PROMO%'
            then l_extendedprice * (1 - l_discount)
        else 0
    end) / sum(l_extendedprice * (1 - l_discount)) as promo_revenue
from
    lineitem,
    part
where
    l_partkey = p_partkey
    and l_shipdate >= m_date
    and l_shipdate < m_date + interval '1' month;

end
""",
"""
create procedure Q15(
    m_date date
)
begin

with m_revenue (supplier_no, total_revenue) as (
    select
        l_suppkey,
        sum(l_extendedprice * (1 - l_discount))
    from
        lineitem
    where
        l_shipdate >= m_date
        and l_shipdate < m_date + interval '3' month
    group by
        l_suppkey
)

select
    s_suppkey,
    s_name,
    s_address,
    s_phone,
    total_revenue
from
    supplier,
    m_revenue
where
    s_suppkey = supplier_no
    and total_revenue = (
        select
            max(total_revenue)
        from
            m_revenue
    )
order by
    s_suppkey;

end
""",
"""
create procedure Q16(
    m_brand char(10),
    m_type varchar(25),
    m_size1 int,
    m_size2 int,
    m_size3 int,
    m_size4 int,
    m_size5 int,
    m_size6 int,
    m_size7 int,
    m_size8 int
)
begin

select
    p_brand,
    p_type,
    p_size,
    count(distinct ps_suppkey) as supplier_cnt
from
    partsupp,
    part
where
    p_partkey = ps_partkey
    and p_brand <> m_brand
    and p_type not like concat(m_type, '%')
    and p_size in (m_size1, m_size2, m_size3, m_size4, m_size5, m_size6, m_size7, m_size8)
    and ps_suppkey not in (
        select
            s_suppkey
        from
            supplier
        where
        s_comment like '%Customer%Complaints%'
    )
group by
    p_brand,
    p_type,
    p_size
order by
    supplier_cnt desc,
    p_brand,
    p_type,
    p_size;

end
""",
"""
create procedure Q17(
    m_brand char(10),
    m_container char(10)
)
begin

select
    sum(l_extendedprice) / 7.0 as avg_yearly
from
    lineitem,
    part
where
    p_partkey = l_partkey
    and p_brand = m_brand
    and p_container = m_container
    and l_quantity < (
        select
            0.2 * avg(l_quantity)
        from
            lineitem
        where
            l_partkey = p_partkey
    );

end
""",
"""
create procedure Q18(
    m_quantity int
)
begin

select
    c_name,
    c_custkey,
    o_orderkey,
    o_orderdate,
    o_totalprice,
    sum(l_quantity)
from
    customer,
    orders,
    lineitem
where
    o_orderkey in (
        select
            l_orderkey
        from
            lineitem
        group by
            l_orderkey having
                sum(l_quantity) > m_quantity
    )
    and c_custkey = o_custkey
    and o_orderkey = l_orderkey
group by
    c_name,
    c_custkey,
    o_orderkey,
    o_orderdate,
    o_totalprice
order by
    o_totalprice desc,
    o_orderdate
limit 100;

end
""",
"""
create procedure Q19(
    m_brand1 char(10),
    m_brand2 char(10),
    m_brand3 char(10),
    m_quantity1 int,
    m_quantity2 int,
    m_quantity3 int
)
begin

select
    sum(l_extendedprice * (1 - l_discount) ) as revenue
from
    lineitem,
    part
where
    (
        p_partkey = l_partkey
        and p_brand = m_brand1
        and p_container in ( 'SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
        and l_quantity >= m_quantity1 and l_quantity <= m_quantity1 + 10
        and p_size between 1 and 5
        and l_shipmode in ('AIR', 'AIR REG')
        and l_shipinstruct = 'DELIVER IN PERSON'
    )
    or
    (
        p_partkey = l_partkey
        and p_brand = m_brand2
        and p_container in ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
        and l_quantity >= m_quantity2 and l_quantity <= m_quantity2 + 10
        and p_size between 1 and 10
        and l_shipmode in ('AIR', 'AIR REG')
        and l_shipinstruct = 'DELIVER IN PERSON'
    )
    or
    (
        p_partkey = l_partkey
        and p_brand = m_brand3
        and p_container in ( 'LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
        and l_quantity >= m_quantity3 and l_quantity <= m_quantity3 + 10
        and p_size between 1 and 15
        and l_shipmode in ('AIR', 'AIR REG')
        and l_shipinstruct = 'DELIVER IN PERSON'
     );

end
""",
"""
create procedure Q20(
    m_color varchar(55),
    m_date date,
    m_nation char(25)
)
begin

select
    s_name,
    s_address
from
    supplier, 
    nation
where
    s_suppkey in (
        select
            ps_suppkey
        from
            partsupp
        where
            ps_partkey in (
                select
                    p_partkey
                from
                    part
                where
                    p_name like concat(m_color, '%')
            )
            and ps_availqty > (
                select
                    0.5 * sum(l_quantity)
                from
                    lineitem
                where
                    l_partkey = ps_partkey
                    and l_suppkey = ps_suppkey
                    and l_shipdate >= m_date
                    and l_shipdate < m_date + interval '1' year
            )
    )
    and s_nationkey = n_nationkey
    and n_name = m_nation
order by
    s_name;

end
""",
"""
create procedure Q21(
    m_nation char(25)
)
begin

select
    s_name,
    count(*) as numwait
from
    supplier,
    lineitem l1,
    orders,
    nation
where
    s_suppkey = l1.l_suppkey
    and o_orderkey = l1.l_orderkey
    and o_orderstatus = 'F'
    and l1.l_receiptdate > l1.l_commitdate
    and exists (
        select
            *
        from
            lineitem l2
        where
            l2.l_orderkey = l1.l_orderkey
            and l2.l_suppkey <> l1.l_suppkey
    )
    and not exists (
        select
            *
        from
            lineitem l3
        where
            l3.l_orderkey = l1.l_orderkey
            and l3.l_suppkey <> l1.l_suppkey
            and l3.l_receiptdate > l3.l_commitdate
    )
    and s_nationkey = n_nationkey
    and n_name = m_nation
group by
    s_name
order by
    numwait desc,
    s_name
limit 100;

end
""",
"""
create procedure Q22(
    m_code1 int,
    m_code2 int,
    m_code3 int,
    m_code4 int,
    m_code5 int,
    m_code6 int,
    m_code7 int
)
begin

select
    cntrycode,
    count(*) as numcust,
    sum(c_acctbal) as totacctbal
from (
    select
        SUBSTRING(c_phone FROM 1 FOR 2) as cntrycode,
        c_acctbal
    from
        customer
    where
        SUBSTRING(c_phone FROM 1 FOR 2) in
            (m_code1, m_code2, m_code3, m_code4, m_code5, m_code6, m_code7)
        and c_acctbal > (
            select
                avg(c_acctbal)
            from
                customer
            where
                c_acctbal > 0.00
                and SUBSTRING(c_phone FROM 1 FOR 2) in
                    (m_code1, m_code2, m_code3, m_code4, m_code5, m_code6, m_code7)
        )
        and not exists (
            select
                *
            from
                orders
            where
                o_custkey = c_custkey
        )
    ) as custsale
group by
    cntrycode
order by
    cntrycode;

end
"""
]

# calling procedures
queries = [
"""
Q0
""",
"""
call Q1(%d);
""",
"""
call Q2(%d, \"%s\", \"%s\");
""",
"""
call Q3(\"%s\", \"%s\");
""",
"""
call Q4(\"%s\");
""",
"""
call Q5(\"%s\", \"%s\");
""",
"""
call Q6(\"%s\", %.02f, %d);
""",
"""
call Q7(\"%s\", \"%s\");
""",
"""
call Q8(\"%s\", \"%s\", \"%s\");
""",
"""
call Q9(\"%s\");
""",
"""
call Q10(\"%s\");
""",
"""
call Q11(\"%s\", %.10f);
""",
"""
call Q12(\"%s\", \"%s\", \"%s\");
""",
"""
call Q13(\"%s\", \"%s\");
""",
"""
call Q14(\"%s\");
""",
"""
call Q15(\"%s\");
""",
"""
call Q16(\"%s\", \"%s\", %d, %d, %d, %d, %d, %d, %d, %d);
""",
"""
call Q17(\"%s\", \"%s\");
""",
"""
call Q18(%d);
""",
"""
call Q19(\"%s\", \"%s\", \"%s\", %d, %d, %d);
""",
"""
call Q20(\"%s\", \"%s\", \"%s\");
""",
"""
call Q21(\"%s\");
""",
"""
call Q22(%d, %d, %d, %d, %d, %d, %d);
"""
]

# (R_REGIONKEY, R_NAME)
regions = [(0, "AFRICA"), (1, "AMERICA"), (2, "ASIA"), 
           (3, "EUROPE"), (4, "MIDDLE_ASIA")]    

R_REGIONKEY = 0
R_NAME = 1

# (N_NATIONKEY, N_NAME, N_REGIONKEY)
nations = [(0, "ALGERIA", 0), (1, "ARGENTINA", 1), (2, "BRAZIL", 1),
           (3, "CANADA", 1), (4, "EGYPT", 4), (5, "ETHIOPIA", 0),
           (6, "FRANCE", 3), (7, "GERMANY", 3), (8, "INDIA", 2),
           (9, "INDONESIA", 2), (10, "IRAN", 4), (11, "IRAQ", 4),
           (12, "JAPAN", 2), (13, "JORDAN", 4), (14, "KENYA", 0),
           (15, "MOROCCO", 0), (16, "MOZAMBIQUE", 0), (17, "PERU", 1),
           (18, "CHINA", 2), (19, "ROMANIA", 3), (20, "SAUDI ARABIA", 4),
           (21, "VIETNAM", 2), (22, "RUSSIA", 3), (23, "UNITED KINGDOM", 3),
           (24, "UNITED STATES", 1)]

N_NATIONKEY = 0
N_NAME = 1
N_REGIONKEY = 2

# (SYLLABLE_1, SYLLABLE_2, SYLLABLE_3)
types = [["STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY"], 
         ["ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"],
         ["TIN", "NICKEL", "BRASS", "STEEL", "COPPER"]]

SYLLABLE_1 = 0
SYLLABLE_2 = 1
SYLLABLE_3 = 2

containers = [["SM", "LG", "MED", "JUMBO", "WRAP"],
              ["CASE", "BOX", "BAG", "JAR", "PKG", "PACK", "CAN", "DRUM"]]

P_NAME = ["almond", "antique", "aquamarine", "azure", "beige", "bisque",
          "black", "blanched", "blue", "blush", "brown", "burlywood", 
          "burnished", "chartreuse", "chiffon", "chocolate", "coral",
          "cornflower", "cornsilk", "cream", "cyan", "dark", "deep", 
          "dim", "dodger", "drab", "firebrick", "floral", "forest", 
          "frosted", "gainsboro", "ghost", "goldenrod", "green", "grey", 
          "honeydew", "hot", "indian", "ivory", "khaki", "lace", 
          "lavender", "lawn", "lemon", "light", "lime", "linen",
          "magenta", "maroon", "medium", "metallic", "midnight", "mint", 
          "misty", "moccasin", "navajo", "navy", "olive", "orange", 
          "orchid", "pale", "papaya", "peach", "peru", "pink", "plum", 
          "powder", "puff", "purple", "red", "rose", "rosy", "royal", 
          "saddle", "salmon", "sandy", "seashell", "sienna", "sky", 
          "slate", "smoke", "snow", "spring", "steel", "tan", "thistle",
          "tomato", "turquoise", "violet", "wheat", "white", "yellow"]

modes = ["REG AIR", "AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB"]

WORD_1 = ["special", "pending", "unusual", "express"]
WORD_2 = ["packages", "requests", "accounts", "deposits"]

segments = ["AUTOMOBILE", "BUILDING", "FURNITURE", "MACHINERY", "HOUSEHOLD"]


def random_date(start_date, end_date):
    """
    This function will return a random datetime between two datetime objects.
    """
    start_timestamp = int(start_date.timestamp())
    end_timestamp = int(end_date.timestamp())
    random_timestamp = random.randint(start_timestamp, \
                            end_timestamp + 24 * 60 * 60 - 1)
    return datetime.fromtimestamp(random_timestamp).strftime("%Y-%m-%d")

def random_date_with_first_day(start_date, end_date):
    """
    This function will return a random datetime between two datetime objects.
    """
    start_timestamp = int(start_date.timestamp())
    end_timestamp = int(end_date.timestamp())
    random_timestamp = random.randint(start_timestamp, \
                            end_timestamp + 24 * 60 * 60 - 1)
    return datetime.fromtimestamp(random_timestamp).replace(day=1).strftime("%Y-%m-%d")

def random_date_with_first_month_and_first_day(start_date, end_date):
    """
    This function will return a random datetime between two datetime objects.
    """
    start_timestamp = int(start_date.timestamp())
    end_timestamp = int(end_date.timestamp())
    random_timestamp = random.randint(start_timestamp, \
                            end_timestamp + 24 * 60 * 60 - 1)
    return datetime.fromtimestamp(random_timestamp).replace(month=1).replace(day=1).strftime("%Y-%m-%d")

def get_procs(query_no):
    proc_template = procs[query_no]

    return proc_template

def get_query_template(query_no):
    query_template = queries[query_no]

    return query_template

def get_query_parameters(query_no, scale_factor):      
    if query_no == 1:
        delta = random.randint(60, 120)
        return [delta]
    elif query_no == 2:
        size = random.randint(1, 50)
        type = random.choice(types[SYLLABLE_3]);
        region = random.choice(regions)[1];
        return [size, type, region]
    elif query_no == 3:
        segment = random.choice(segments)
        date = random_date(datetime(1995, 3, 1), datetime(1995, 3, 31))
        return [segment, date]
    elif query_no == 4:
        date = random_date_with_first_day(datetime(1993, 1, 1), datetime(1997, 10, 31))
        return [date]
    elif query_no == 5:
        region = random.choice(regions)[R_NAME]
        date = random_date_with_first_month_and_first_day(datetime(1993, 1, 1), datetime(1997, 12, 31))
        return [region, date]
    elif query_no == 6:
        date = random_date_with_first_month_and_first_day(datetime(1993, 1, 1), datetime(1997, 12, 31))
        discount = random.randint(2, 9) / 100
        quantity = random.randint(24, 25)
        return [date, discount, quantity]
    elif query_no == 7:
        nation_list = random.sample(nations, 2)
        nation_1 = nation_list[0][N_NAME]
        nation_2 = nation_list[1][N_NAME]
        return [nation_1, nation_2]
    elif query_no == 8:
        nation_tup = random.choice(nations)

        nation = nation_tup[N_NAME]
        region = regions[nation_tup[N_REGIONKEY]][R_NAME]
        type = f"{random.choice(types[SYLLABLE_1])} {random.choice(types[SYLLABLE_2])} {random.choice(types[SYLLABLE_3])}"
        return [nation, region, type]
    elif query_no == 9:
        color = random.choice(P_NAME)
        return [color]
    elif query_no == 10:
        date = random_date_with_first_day(datetime(1993, 2, 1), datetime(1995, 1, 31))
        return [date]
    elif query_no == 11:
        nation = random.choice(nations)[N_NAME]
        fraction = 0.0001 / float(scale_factor)
        return [nation, fraction]
    elif query_no == 12:
        mode_list = random.sample(modes, 2)
        mode_1 = mode_list[0]
        mode_2 = mode_list[1]
        date = random_date_with_first_month_and_first_day(datetime(1993, 1, 1), datetime(1997, 12, 31))
        return [mode_1, mode_2, date]
    elif query_no == 13:
        word_1 = random.choice(WORD_1)
        word_2 = random.choice(WORD_2)
        return [word_1, word_2]
    elif query_no == 14:
        date = random_date_with_first_day(datetime(1993, 1, 1), datetime(1997, 12, 31))
        return [date]
    elif query_no == 15:
        date = random_date_with_first_day(datetime(1993, 1, 1), datetime(1997, 10, 31))
        return [date]
    elif query_no == 16:
        brand = f"Brand#{random.randint(1, 5)}{random.randint(1, 5)}"
        type = f"{random.choice(types[SYLLABLE_1])} {random.choice(types[SYLLABLE_2])}"
        size_tup = random.sample(list(range(1, 51)), 8)
        size_1 = size_tup[0]
        size_2 = size_tup[1]
        size_3 = size_tup[2]
        size_4 = size_tup[3]
        size_5 = size_tup[4]
        size_6 = size_tup[5]
        size_7 = size_tup[6]
        size_8 = size_tup[7]
        return [brand, type, size_1, size_2, size_3, size_4, size_5, size_6, size_7, size_8]
    elif query_no == 17:
        brand = f"Brand#{random.randint(1, 5)}{random.randint(1, 5)}"
        container = f"{random.choice(containers[SYLLABLE_1])} {random.choice(containers[SYLLABLE_2])}"
        return [brand, container]
    elif query_no == 18:
        quantity = random.randint(312, 315)
        return [quantity]
    elif query_no == 19:
        quantity_1 = random.randint(1, 10)
        quantity_2 = random.randint(10, 20)
        quantity_3 = random.randint(20, 30)
        brand_1 = f"Brand#{random.randint(1, 5)}{random.randint(1, 5)}"
        brand_2 = f"Brand#{random.randint(1, 5)}{random.randint(1, 5)}"
        brand_3 = f"Brand#{random.randint(1, 5)}{random.randint(1, 5)}"
        return [brand_1, brand_2, brand_3, quantity_1, quantity_2, quantity_3]
    elif query_no == 20:
        color = random.choice(P_NAME)
        date = random_date_with_first_month_and_first_day(datetime(1993, 1, 1), datetime(1997, 12, 31))
        nation = random.choice(nations)[N_NAME]
        return [color, date, nation]
    elif query_no == 21:
        nation = random.choice(nations)[N_NAME]
        return [nation]
    elif query_no == 22:
        i_tup = random.sample(list(range(10, 35)), 7)
        i_1 = i_tup[0]
        i_2 = i_tup[1]
        i_3 = i_tup[2]
        i_4 = i_tup[3]
        i_5 = i_tup[4]
        i_6 = i_tup[5]
        i_7 = i_tup[6]
        return [i_1, i_2, i_3, i_4, i_5, i_6, i_7]

def get_query(query_no, scale_factor):
    query_template =get_query_template(query_no)
    query_params = get_query_parameters(query_no, scale_factor)

    return query_template % tuple(query_params)

